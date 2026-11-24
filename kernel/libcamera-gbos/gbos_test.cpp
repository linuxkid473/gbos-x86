/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * gbOS test camera: a libcamera pipeline-handler plugin for Google's Android camera HAL
 * (camera.libcamera_hal_base.so, loaded by the vendor's ICameraProvider/legacy/0 service). It
 * shows moving colour bars, so one boot proves that our plugins load in that HAL and reach
 * Android apps. Modelled on libcamera's virtual pipeline handler
 * (src/libcamera/pipeline/virtual, Copyright (C) 2024, Google Inc.), written against the
 * headers of AOSP external/libcamera android17-release.
 *
 * The HAL dlopen()s every .so in /vendor/lib64/libcamera/pipeline; REGISTER_PIPELINE_HANDLER
 * below runs from this library's constructors.
 *
 * The camera is INTERNAL, id "camera1", which the image's camera_hal.yaml maps to the back
 * camera. Boot-16 (2026-11-20): as an external camera it crash-looped the provider. The HAL's
 * CameraHalManager::setCallbacks() reports every external camera PRESENT at once, and Google's
 * adapter (AidlCameraProvider::Create) calls setCallbacks before its provider object exists:
 * HandleCameraDeviceStatusChange dereferenced null, 23 crashes in 2 minutes. Internal cameras
 * get no such callback. So no gbOS plugin may register an external camera, or add one after
 * start (a hotplug is reported the same way).
 */

#include <algorithm>
#include <chrono>
#include <errno.h>
#include <map>
#include <memory>
#include <set>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include <libcamera/base/log.h>
#include <libcamera/control_ids.h>
#include <libcamera/controls.h>
#include <libcamera/formats.h>
#include <libcamera/property_ids.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include "libcamera/internal/camera.h"
#include "libcamera/internal/dma_heaps.h"
#include "libcamera/internal/formats.h"
#include "libcamera/internal/framebuffer.h"
#include "libcamera/internal/mapped_framebuffer.h"
#include "libcamera/internal/pipeline_handler.h"

namespace libcamera {

LOG_DEFINE_CATEGORY(GbosTest)

namespace {

const Size kSizes[] = {
	{ 640, 480 }, { 1280, 720 }, { 1280, 960 }, { 1920, 1080 }, { 2560, 1920 },
};
constexpr unsigned int kBufferCount = 4;
constexpr int64_t kFrameDurationUs = 33333;

uint64_t nowNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

Size largestSize()
{
	return kSizes[std::size(kSizes) - 1];
}

bool supported(const Size &size)
{
	return std::find(std::begin(kSizes), std::end(kSizes), size) != std::end(kSizes);
}

} /* namespace */

class GbosTestCameraData : public Camera::Private
{
public:
	static constexpr unsigned int kMaxStreams = 3;

	GbosTestCameraData(PipelineHandler *pipe)
		: Camera::Private(pipe), streams_(kMaxStreams)
	{
		const Size max = largestSize();
		properties_.set(properties::Location, properties::CameraLocationBack);
		properties_.set(properties::Rotation, 0);
		properties_.set(properties::Model, "gbOS test camera");
		properties_.set(properties::PixelArraySize, max);
		properties_.set(properties::PixelArrayActiveAreas, { Rectangle(max) });

		/*
		 * Google's HAL claims FULL level for every libcamera camera (camera_capabilities.cpp:
		 * "force switch to full mode"), so it also claims MANUAL_SENSOR. Boot-17: the camera app
		 * stalled before opening any camera; without ExposureTime the HAL leaves out
		 * SENSOR_INFO_EXPOSURE_TIME_RANGE, which a FULL camera must have. AnalogueGain stays out:
		 * the HAL then reports a valid default sensitivity range (32..2400).
		 */
		ControlInfoMap::Map controls;
		controls[&controls::FrameDurationLimits] =
			ControlInfo(kFrameDurationUs, kFrameDurationUs, kFrameDurationUs);
		controls[&controls::ExposureTime] = ControlInfo(100, 66666, 33333);
		controlInfo_ = ControlInfoMap(std::move(controls), controls::controls);
	}

	std::vector<Stream> streams_;
	unsigned int frame_ = 0;
};

class GbosTestCameraConfiguration : public CameraConfiguration
{
public:
	Status validate() override
	{
		if (config_.empty())
			return Invalid;
		Status status = Valid;
		if (config_.size() > GbosTestCameraData::kMaxStreams) {
			config_.resize(GbosTestCameraData::kMaxStreams);
			status = Adjusted;
		}
		for (StreamConfiguration &cfg : config_) {
			if (!supported(cfg.size)) {
				cfg.size = largestSize();
				status = Adjusted;
			}
			if (cfg.pixelFormat != formats::NV12) {
				cfg.pixelFormat = formats::NV12;
				status = Adjusted;
			}
			const PixelFormatInfo &info = PixelFormatInfo::info(cfg.pixelFormat);
			cfg.stride = info.stride(cfg.size.width, 0, 1);
			cfg.frameSize = info.frameSize(cfg.size, 1);
			cfg.bufferCount = kBufferCount;
		}
		return status;
	}
};

class PipelineHandlerGbosTest : public PipelineHandler
{
public:
	PipelineHandlerGbosTest(CameraManager *manager)
		: PipelineHandler(manager)
	{
	}

	std::unique_ptr<CameraConfiguration>
	generateConfiguration(Camera *camera, Span<const StreamRole> roles) override;
	int configure(Camera *camera, CameraConfiguration *config) override;
	int exportFrameBuffers(Camera *camera, Stream *stream,
			       std::vector<std::unique_ptr<FrameBuffer>> *buffers) override;
	int start(Camera *camera, const ControlList *controls) override;
	void stopDevice(Camera *camera) override;
	int queueRequestDevice(Camera *camera, Request *request) override;
	bool match(DeviceEnumerator *enumerator) override;

private:
	GbosTestCameraData *cameraData(Camera *camera)
	{
		return static_cast<GbosTestCameraData *>(camera->_d());
	}
	void fill(FrameBuffer *buffer, const Size &size, unsigned int frame);

	static bool created_;
	/* Opened on first use: the HAL process may not open /dev/dma_heap/system (boot-16). */
	std::unique_ptr<DmaHeap> dmaHeap_;
};

bool PipelineHandlerGbosTest::created_ = false;

std::unique_ptr<CameraConfiguration>
PipelineHandlerGbosTest::generateConfiguration([[maybe_unused]] Camera *camera,
					       Span<const StreamRole> roles)
{
	auto config = std::make_unique<GbosTestCameraConfiguration>();

	for (const StreamRole role : roles) {
		if (role == StreamRole::Raw) {
			LOG(GbosTest, Error) << "Raw streams are not supported";
			return nullptr;
		}
		std::map<PixelFormat, std::vector<SizeRange>> formats;
		for (const Size &size : kSizes)
			formats[formats::NV12].push_back(SizeRange(size));
		StreamConfiguration cfg{ StreamFormats(formats) };
		cfg.pixelFormat = formats::NV12;
		cfg.size = role == StreamRole::StillCapture ? largestSize() : Size(1280, 960);
		cfg.bufferCount = kBufferCount;
		config->addConfiguration(cfg);
	}
	if (!roles.empty() && config->validate() == CameraConfiguration::Invalid)
		return nullptr;
	return config;
}

int PipelineHandlerGbosTest::configure(Camera *camera, CameraConfiguration *config)
{
	GbosTestCameraData *data = cameraData(camera);

	for (unsigned int i = 0; i < config->size(); i++)
		config->at(i).setStream(&data->streams_[i]);
	return 0;
}

int PipelineHandlerGbosTest::exportFrameBuffers([[maybe_unused]] Camera *camera, Stream *stream,
						std::vector<std::unique_ptr<FrameBuffer>> *buffers)
{
	if (!dmaHeap_)
		dmaHeap_ = std::make_unique<DmaHeap>();
	if (!dmaHeap_->isValid())
		return -ENOBUFS;
	const StreamConfiguration &config = stream->configuration();
	const PixelFormatInfo &info = PixelFormatInfo::info(config.pixelFormat);
	std::vector<unsigned int> planeSizes;
	for (size_t i = 0; i < info.planes.size(); i++)
		planeSizes.push_back(info.planeSize(config.size, i));
	return dmaHeap_->exportBuffers(config.bufferCount, planeSizes, buffers, DmaHeap::Type::System);
}

int PipelineHandlerGbosTest::start(Camera *camera, [[maybe_unused]] const ControlList *controls)
{
	cameraData(camera)->frame_ = 0;
	LOG(GbosTest, Info) << "gbOS test camera started";
	return 0;
}

void PipelineHandlerGbosTest::stopDevice([[maybe_unused]] Camera *camera)
{
}

/* Eight colour bars scrolling one bar width every 64 frames, and a grey ramp at the bottom. */
void PipelineHandlerGbosTest::fill(FrameBuffer *buffer, const Size &size, unsigned int frame)
{
	static const uint8_t bars[8][3] = {	/* Y, U, V (BT.601 limited range) */
		{ 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
		{ 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 },
	};
	MappedFrameBuffer mapped(buffer, MappedFrameBuffer::MapFlag::Write);
	if (!mapped.isValid()) {
		LOG(GbosTest, Error) << "Cannot map the frame buffer";
		return;
	}
	auto planes = mapped.planes();
	if (planes.empty())
		return;
	/* Stride from the luma plane's length when it is padded (gralloc), else the width. */
	unsigned int stride = size.width;
	if (planes[0].size() >= (size_t)size.width * size.height * 3 / 2 && planes.size() == 1)
		stride = planes[0].size() * 2 / 3 / size.height;
	else if (planes[0].size() >= (size_t)size.width * size.height)
		stride = planes[0].size() / size.height;
	uint8_t *y = planes[0].data();
	uint8_t *uv = planes.size() > 1 ? planes[1].data() : y + (size_t)stride * size.height;
	unsigned int barWidth = std::max(1u, size.width / 8), shift = frame * barWidth / 64;

	for (unsigned int row = 0; row < size.height; row++) {
		uint8_t *line = y + (size_t)row * stride;
		bool ramp = row >= size.height * 7 / 8;
		for (unsigned int col = 0; col < size.width; col++)
			line[col] = ramp ? (uint8_t)(16 + 219 * col / size.width)
					 : bars[((col + shift) / barWidth) % 8][0];
	}
	for (unsigned int row = 0; row < size.height / 2; row++) {
		uint8_t *line = uv + (size_t)row * stride;
		bool ramp = row * 2 >= size.height * 7 / 8;
		for (unsigned int col = 0; col < size.width / 2; col++) {
			const uint8_t *c = bars[((col * 2 + shift) / barWidth) % 8];
			line[col * 2] = ramp ? 128 : c[1];
			line[col * 2 + 1] = ramp ? 128 : c[2];
		}
	}
}

int PipelineHandlerGbosTest::queueRequestDevice(Camera *camera, Request *request)
{
	GbosTestCameraData *data = cameraData(camera);
	ControlList metadata;

	metadata.set(controls::SensorTimestamp, static_cast<int64_t>(nowNs()));
	metadata.set(controls::FrameDuration, kFrameDurationUs);
	metadataAvailable(request, metadata);
	for (auto const &[stream, buffer] : request->buffers()) {
		fill(buffer, stream->configuration().size, data->frame_);
		completeBuffer(request, buffer);
	}
	data->frame_++;
	completeRequest(request);
	return 0;
}

bool PipelineHandlerGbosTest::match([[maybe_unused]] DeviceEnumerator *enumerator)
{
	if (created_)
		return false;
	created_ = true;

	auto data = std::make_unique<GbosTestCameraData>(this);
	std::set<Stream *> streams;
	for (Stream &stream : data->streams_)
		streams.insert(&stream);
	std::shared_ptr<Camera> camera = Camera::create(std::move(data), "camera1", streams);
	registerCamera(std::move(camera));
	LOG(GbosTest, Info) << "gbOS test camera registered";
	return true;
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerGbosTest)

} /* namespace libcamera */
