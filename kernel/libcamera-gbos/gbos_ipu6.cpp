/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * gbOS IPU6 cameras for Google's Android libcamera HAL (camera.libcamera_hal_base.so, loaded by
 * the vendor's ICameraProvider/legacy/0 service, which dlopen()s every .so in
 * /vendor/lib64/libcamera/pipeline). Written against the headers of AOSP external/libcamera
 * android17-release; structure as gbos_test.cpp (the test camera that proved the plugin path).
 *
 * The sensor, its auto exposure, white balance and focus, and the conversion of raw frames to
 * a picture stay in the ipu6-loopback bridge, a separate process (kernel/ipu6-loopback.c). With
 * the output "channel:<camera id>" it hands each converted frame to this plugin over the frame
 * channel (kernel/ipu6-channel.h): a picture in shared memory plus the frame's exposure and
 * timestamp. The plugin turns that picture into every stream the app asks for (NV12 at any
 * size up to the bridge's, centre-cropped to the stream's aspect), so one camera serves
 * preview, video and stills at once, which the single-format v4l2loopback route could not.
 * Keeping the sensor work out of this process also means a bridge failure never takes the camera
 * service down: the plugin just waits for the bridge to come back.
 *
 * Cameras: INTERNAL "camera0", the front OV5678 (RGB-IR), and "camera1", the rear OV8856, as
 * Google's camera_hal.yaml names them (camera0 front, camera1 back, rotation 0). Each is registered
 * only when its bridge's channel exists, so CAMERA_LIBCAMERA=1 (rear only; the front stays on the
 * USB HAL) and CAMERA_LIBCAMERA=both use the same plugin. Internal only: an external camera crashes
 * Google's adapter (boot-16, see gbos_test.cpp).
 *
 * Sizes: CameraX in Google's camera app takes only its video qualities when there are no encoder
 * profiles (SD 640x480, HD 1280x720, FHD 1920x1080; boot-17/18), and all three are listed.
 */

#include <algorithm>
#include <deque>
#include <errno.h>
#include <fstream>
#include <linux/dma-buf.h>
#include <map>
#include <memory>
#include <poll.h>
#include <set>
#include <stdint.h>
#include <string.h>
#include <string>
#include <typeinfo>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <libcamera/base/event_notifier.h>
#include <libcamera/base/log.h>
#include <libcamera/base/timer.h>
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

#include "../ipu6-channel.h"
#include "gbos_convert.h"

/*
 * Google's HAL hands us gralloc buffers as HALFrameBuffer (src/android/hal_framebuffer.h: a
 * FrameBuffer plus its buffer_handle_t), and its minigbm buffer manager knows their real row
 * strides; both are exported by camera.libcamera_hal_base.so. Declared here as there (the AOSP
 * headers need Android's cutils/hardware headers, which the NDK lacks); only the layout and the
 * mangled names matter. A buffer is recognised by its type name, so no typeinfo of ours is
 * compared with the vendor's.
 */
struct native_handle;
typedef const native_handle *buffer_handle_t;

namespace crosLibcamera {
class CameraBufferManager
{
public:
	static size_t GetPlaneStride(buffer_handle_t buffer, size_t plane);
};
} /* namespace crosLibcamera */

class HALFrameBuffer final : public libcamera::FrameBuffer
{
public:
	buffer_handle_t handle() const { return handle_; }

private:
	HALFrameBuffer() = delete;
	buffer_handle_t handle_;
};

namespace libcamera {

LOG_DEFINE_CATEGORY(GbosIpu6)

namespace {

/* What the libcamera-mode bridge build delivers (installer/bin/1920x1440/): both sensors are 4:3. */
const Size kMaxSize{ 1920, 1440 };
/* Listed sizes (the HAL adds camera3Resolutions below the maximum and the maximum itself). */
const Size kSizes[] = {
	{ 640, 480 }, { 1280, 720 }, { 1280, 960 }, { 1440, 1080 }, { 1920, 1080 }, { 1920, 1440 },
};
constexpr unsigned int kBufferCount = 4;
constexpr int64_t kFrameDurationUs = 33333;
/* Preview + YUV analysis + video + the HAL's internal still stream for JPEG. */
constexpr unsigned int kMaxStreams = 4;

bool sizeOk(const Size &s)
{
	return s.width >= 2 && s.height >= 2 && !(s.width & 1) && !(s.height & 1) &&
	       s.width <= kMaxSize.width && s.height <= kMaxSize.height;
}

/* The bridge listens on "\0gbos-camera:<id>"; /proc/net/unix lists it as "@gbos-camera:<id>". */
bool channelListening(const std::string &id)
{
	std::ifstream f("/proc/net/unix");
	std::string line, want = "@" IPU6_CHANNEL_PREFIX + id;

	while (std::getline(f, line)) {
		size_t at = line.rfind(' ');
		if (at != std::string::npos && line.compare(at + 1, std::string::npos, want) == 0)
			return true;
	}
	return false;
}

struct CameraInfo {
	const char *id;
	int location;
	const char *model;
};

/*
 * Registration order sets the HAL's internal ids (0, 1, ... as registered; camera_hal_manager.cpp).
 * Google's adapter published the only camera, the back test camera, as device@1.0/legacy/1
 * (boot-17), so its public ids follow the facing or the yaml name rather than that order. The
 * USB provider has no cameras when both are here (CAMERA_LIBCAMERA=both), so nothing can clash.
 */
const CameraInfo kCameras[] = {
	{ "camera0", properties::CameraLocationFront, "OV5678 (gbOS IPU6)" },
	{ "camera1", properties::CameraLocationBack, "OV8856 (gbOS IPU6)" },
};

} /* namespace */

class GbosIpu6CameraData : public Camera::Private
{
public:
	GbosIpu6CameraData(PipelineHandler *pipe, const CameraInfo &info)
		: Camera::Private(pipe), id_(info.id), streams_(kMaxStreams)
	{
		properties_.set(properties::Location, info.location);
		properties_.set(properties::Rotation, 0);	/* as camera_hal.yaml; the rear bridge applies CAMERA_REAR_ROTATION */
		properties_.set(properties::Model, info.model);
		properties_.set(properties::PixelArraySize, kMaxSize);
		properties_.set(properties::PixelArrayActiveAreas, { Rectangle(kMaxSize) });

		/*
		 * As gbos_test.cpp: Google's HAL claims FULL (so MANUAL_SENSOR) for every libcamera
		 * camera and needs an ExposureTime range; without AnalogueGain it reports a default
		 * sensitivity range. Exposure stays automatic (the bridge's AE); manual values are
		 * not applied yet.
		 */
		ControlInfoMap::Map controls;
		controls[&controls::FrameDurationLimits] =
			ControlInfo(kFrameDurationUs, kFrameDurationUs, kFrameDurationUs);
		controls[&controls::ExposureTime] = ControlInfo(100, 66666, 33333);
		controlInfo_ = ControlInfoMap(std::move(controls), controls::controls);
	}

	~GbosIpu6CameraData()
	{
		disconnect();
	}

	int connect();
	void disconnect();

	std::string id_;
	std::vector<Stream> streams_;
	bool running_ = false;

	int sock_ = -1;
	const uint8_t *map_ = nullptr;
	size_t mapBytes_ = 0;
	ipu6_channel_hello hello_{};
	std::unique_ptr<EventNotifier> notifier_;
	std::unique_ptr<Timer> retry_;
	std::deque<Request *> pending_;
	uint64_t frames_ = 0, dropped_ = 0;
};

/* Connect to the bridge and map its pictures. 0, or -errno (the caller retries). */
int GbosIpu6CameraData::connect()
{
	struct sockaddr_un a = {};
	a.sun_family = AF_UNIX;
	int n = snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "%s%s", IPU6_CHANNEL_PREFIX, id_.c_str());
	socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + n;

	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (s < 0)
		return -errno;
	if (::connect(s, reinterpret_cast<sockaddr *>(&a), len)) {
		int e = -errno;
		close(s);
		return e;
	}
	/* The hello comes at once (the bridge accepts within a frame or a second when stopped). */
	struct pollfd p = { s, POLLIN, 0 };
	if (poll(&p, 1, 2000) != 1) {
		close(s);
		return -ETIMEDOUT;
	}
	ipu6_channel_hello h{};
	char control[CMSG_SPACE(sizeof(int))] = {};
	struct iovec iov = { &h, sizeof(h) };
	struct msghdr m = {};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = control;
	m.msg_controllen = sizeof(control);
	ssize_t got = recvmsg(s, &m, MSG_CMSG_CLOEXEC);
	int memfd = -1;
	for (cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(&memfd, CMSG_DATA(c), sizeof(int));
	uint64_t need = (uint64_t)h.header_bytes + (uint64_t)h.slots * h.slot_bytes;
	if (got != (ssize_t)sizeof(h) || memfd < 0 || h.magic != IPU6_CHANNEL_MAGIC ||
	    h.version != IPU6_CHANNEL_VERSION || h.slots != IPU6_CHANNEL_SLOTS ||
	    h.width < 2 || h.height < 2 || (h.width & 1) || h.header_bytes < sizeof(ipu6_channel_shared) ||
	    h.slot_bytes < (uint64_t)h.width * h.height * 2 || need > h.map_bytes) {
		LOG(GbosIpu6, Error) << id_ << ": bad frame channel hello";
		if (memfd >= 0)
			close(memfd);
		close(s);
		return -EPROTO;
	}
	void *map = mmap(nullptr, h.map_bytes, PROT_READ, MAP_SHARED, memfd, 0);
	close(memfd);
	if (map == MAP_FAILED) {
		int e = -errno;
		close(s);
		return e;
	}
	sock_ = s;
	map_ = static_cast<const uint8_t *>(map);
	mapBytes_ = h.map_bytes;
	hello_ = h;
	LOG(GbosIpu6, Info) << id_ << ": frame channel connected, " << h.width << "x" << h.height << " YUYV";
	return 0;
}

void GbosIpu6CameraData::disconnect()
{
	notifier_.reset();
	if (map_)
		munmap(const_cast<uint8_t *>(map_), mapBytes_);
	map_ = nullptr;
	if (sock_ >= 0)
		close(sock_);
	sock_ = -1;
}

class GbosIpu6CameraConfiguration : public CameraConfiguration
{
public:
	Status validate() override
	{
		if (config_.empty())
			return Invalid;
		Status status = Valid;
		if (config_.size() > kMaxStreams) {
			config_.resize(kMaxStreams);
			status = Adjusted;
		}
		for (StreamConfiguration &cfg : config_) {
			if (!sizeOk(cfg.size)) {
				Size s = cfg.size.boundedTo(kMaxSize);
				s.width &= ~1u;
				s.height &= ~1u;
				cfg.size = sizeOk(s) ? s : kMaxSize;
				status = Adjusted;
			}
			/*
			 * Google's HAL asks for its JPEG source as an MJPEG StillCapture stream
			 * (buildStreamConfigsNoMap) and treats any Adjusted result as a failure
			 * (boot-21: "Set of requested inputs/outputs not supported by HAL"), yet its
			 * encoder and thumbnailer read NV12, in YCbCr_420_888 buffers it allocates
			 * itself. So MJPEG becomes NV12 without reporting an adjustment.
			 */
			if (cfg.pixelFormat == formats::MJPEG) {
				cfg.pixelFormat = formats::NV12;
			} else if (cfg.pixelFormat != formats::NV12) {
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

class PipelineHandlerGbosIpu6 : public PipelineHandler
{
public:
	PipelineHandlerGbosIpu6(CameraManager *manager)
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
	GbosIpu6CameraData *cameraData(Camera *camera)
	{
		return static_cast<GbosIpu6CameraData *>(camera->_d());
	}
	void tryConnect(GbosIpu6CameraData *data);
	void channelReadable(GbosIpu6CameraData *data);
	void lostChannel(GbosIpu6CameraData *data, const char *why);
	void completeFrame(GbosIpu6CameraData *data, Request *request, const ipu6_channel_frame &f);

	static bool created_;
	std::unique_ptr<DmaHeap> dmaHeap_;
};

bool PipelineHandlerGbosIpu6::created_ = false;

std::unique_ptr<CameraConfiguration>
PipelineHandlerGbosIpu6::generateConfiguration([[maybe_unused]] Camera *camera,
					       Span<const StreamRole> roles)
{
	auto config = std::make_unique<GbosIpu6CameraConfiguration>();

	for (const StreamRole role : roles) {
		if (role == StreamRole::Raw) {
			LOG(GbosIpu6, Error) << "Raw streams are not supported";
			return nullptr;
		}
		std::map<PixelFormat, std::vector<SizeRange>> formats;
		for (const Size &size : kSizes)
			formats[formats::NV12].push_back(SizeRange(size));
		StreamConfiguration cfg{ StreamFormats(formats) };
		cfg.pixelFormat = formats::NV12;
		cfg.size = role == StreamRole::StillCapture ? kMaxSize :
			   role == StreamRole::VideoRecording ? Size(1920, 1080) : Size(1280, 720);
		cfg.bufferCount = kBufferCount;
		config->addConfiguration(cfg);
	}
	if (!roles.empty() && config->validate() == CameraConfiguration::Invalid)
		return nullptr;
	return config;
}

int PipelineHandlerGbosIpu6::configure(Camera *camera, CameraConfiguration *config)
{
	GbosIpu6CameraData *data = cameraData(camera);

	for (unsigned int i = 0; i < config->size(); i++) {
		config->at(i).setStream(&data->streams_[i]);
		LOG(GbosIpu6, Info) << data->id_ << ": stream " << i << " " << config->at(i).toString();
	}
	return 0;
}

int PipelineHandlerGbosIpu6::exportFrameBuffers([[maybe_unused]] Camera *camera, Stream *stream,
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

void PipelineHandlerGbosIpu6::tryConnect(GbosIpu6CameraData *data)
{
	if (!data->running_ || data->sock_ >= 0)
		return;
	int r = data->connect();
	if (r) {
		/* The bridge may be restarting (its supervisor waits 5 s); requests wait meanwhile. */
		if (!data->retry_) {
			data->retry_ = std::make_unique<Timer>();
			data->retry_->timeout.connect(this, [this, data] { tryConnect(data); });
		}
		data->retry_->start(std::chrono::milliseconds(500));
		LOG(GbosIpu6, Debug) << data->id_ << ": frame channel not ready (" << strerror(-r) << "); retrying";
		return;
	}
	data->notifier_ = std::make_unique<EventNotifier>(data->sock_, EventNotifier::Read);
	data->notifier_->activated.connect(this, [this, data] { channelReadable(data); });
}

void PipelineHandlerGbosIpu6::lostChannel(GbosIpu6CameraData *data, const char *why)
{
	LOG(GbosIpu6, Warning) << data->id_ << ": frame channel " << why << "; reconnecting";
	data->disconnect();
	tryConnect(data);
}

void PipelineHandlerGbosIpu6::channelReadable(GbosIpu6CameraData *data)
{
	for (;;) {
		ipu6_channel_frame f;
		ssize_t n = recv(data->sock_, &f, sizeof(f), MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return;
		if (n <= 0) {
			lostChannel(data, n == 0 ? "closed by the bridge" : "failed");
			return;
		}
		if (n != (ssize_t)sizeof(f) || f.magic != IPU6_CHANNEL_MAGIC || f.slot >= data->hello_.slots)
			continue;
		if (data->pending_.empty()) {
			data->dropped_++;	/* no request queued: the app is behind; take the next frame */
			continue;
		}
		Request *request = data->pending_.front();
		data->pending_.pop_front();
		completeFrame(data, request, f);
	}
}

namespace {

/*
 * Bytes per row of an NV12 buffer: the gralloc stride for the HAL's buffers, else (our DmaHeap
 * buffers, packed) the width. The UV plane has the same stride as luma in NV12.
 */
unsigned int rowStride(const FrameBuffer *buffer, size_t plane0, size_t planes, const Size &size)
{
	static const char *const kHal = typeid(HALFrameBuffer).name();
	unsigned int stride = 0;

	if (!strcmp(typeid(*buffer).name(), kHal)) {
		const HALFrameBuffer *hal = static_cast<const HALFrameBuffer *>(buffer);
		stride = (unsigned int)crosLibcamera::CameraBufferManager::GetPlaneStride(hal->handle(), 0);
	}
	if (stride < size.width || stride > size.width * 4u) {
		/* Not a HAL buffer (or no answer): packed, as exportFrameBuffers makes them. */
		stride = size.width;
		if (planes == 1 && plane0 >= (size_t)size.width * size.height * 3 / 2)
			stride = (unsigned int)(plane0 * 2 / 3 / size.height);
		else if (planes > 1 && plane0 >= (size_t)size.width * size.height)
			stride = (unsigned int)(plane0 / size.height);
		stride = std::max(stride & ~1u, size.width);
	}
	return stride;
}

/* CPU access to a dma-buf the GPU or display may also use. */
void dmabufSync(const FrameBuffer *buffer, uint64_t flags)
{
	std::set<int> fds;
	for (const FrameBuffer::Plane &p : buffer->planes())
		fds.insert(p.fd.get());
	for (int fd : fds) {
		struct dma_buf_sync s = { flags | DMA_BUF_SYNC_WRITE };
		ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
	}
}

} /* namespace */

void PipelineHandlerGbosIpu6::completeFrame(GbosIpu6CameraData *data, Request *request,
					    const ipu6_channel_frame &f)
{
	const ipu6_channel_shared *sh = reinterpret_cast<const ipu6_channel_shared *>(data->map_);
	const uint8_t *src = data->map_ + data->hello_.header_bytes + (size_t)f.slot * data->hello_.slot_bytes;
	bool torn = __atomic_load_n(&sh->seq[f.slot], __ATOMIC_ACQUIRE) != f.seq;

	for (auto const &[stream, buffer] : request->buffers()) {
		const Size size = stream->configuration().size;
		dmabufSync(buffer, DMA_BUF_SYNC_START);
		{
			MappedFrameBuffer mapped(buffer, MappedFrameBuffer::MapFlag::Write);
			auto planes = mapped.planes();
			if (!mapped.isValid() || planes.empty()) {
				LOG(GbosIpu6, Error) << data->id_ << ": cannot map a frame buffer";
			} else {
				unsigned int stride = rowStride(buffer, planes[0].size(), planes.size(), size);
				uint8_t *y = planes[0].data();
				uint8_t *uv = planes.size() > 1 ? planes[1].data() : y + (size_t)stride * size.height;
				size_t need = (size_t)stride * (size.height - 1) + size.width;
				size_t needUv = (size_t)stride * (size.height / 2 - 1) + size.width;
				size_t haveUv = planes.size() > 1 ? planes[1].size() :
					planes[0].size() - (size_t)stride * size.height;
				if (planes[0].size() < need || haveUv < needUv) {
					LOG(GbosIpu6, Error) << data->id_ << ": buffer too small for " << size
							     << " at stride " << stride;
				} else {
					gbos_yuyv_to_nv12(src, data->hello_.width, data->hello_.height, y, uv,
							  stride, size.width, size.height);
				}
			}
		}
		dmabufSync(buffer, DMA_BUF_SYNC_END);
	}
	/* A slot rewritten while we read it: the bridge lapped us (4 frames behind). Rare; keep it. */
	if (torn || __atomic_load_n(&sh->seq[f.slot], __ATOMIC_ACQUIRE) != f.seq)
		LOG(GbosIpu6, Debug) << data->id_ << ": frame " << f.seq << " overwritten while copied";

	ControlList metadata;
	metadata.set(controls::SensorTimestamp, static_cast<int64_t>(f.timestamp_ns));
	metadata.set(controls::ExposureTime, f.exposure_us);
	metadata.set(controls::AnalogueGain, f.analogue_gain);
	metadata.set(controls::FrameDuration, static_cast<int64_t>(f.frame_duration_us));
	metadataAvailable(request, metadata);
	for (auto const &[stream, buffer] : request->buffers())
		completeBuffer(request, buffer);
	completeRequest(request);
	if (++data->frames_ == 30 || data->frames_ % 900 == 0)
		LOG(GbosIpu6, Info) << data->id_ << ": " << data->frames_ << " frames delivered, "
				    << data->dropped_ << " bridge frames skipped (no request queued)";
}

int PipelineHandlerGbosIpu6::start(Camera *camera, [[maybe_unused]] const ControlList *controls)
{
	GbosIpu6CameraData *data = cameraData(camera);

	data->running_ = true;
	data->frames_ = data->dropped_ = 0;
	tryConnect(data);
	LOG(GbosIpu6, Info) << data->id_ << ": started"
			    << (data->sock_ >= 0 ? "" : " (waiting for the bridge)");
	return 0;
}

void PipelineHandlerGbosIpu6::stopDevice(Camera *camera)
{
	GbosIpu6CameraData *data = cameraData(camera);

	data->running_ = false;
	if (data->retry_)
		data->retry_->stop();
	data->disconnect();	/* the bridge goes idle */
	while (!data->pending_.empty()) {
		Request *request = data->pending_.front();
		data->pending_.pop_front();
		for (auto const &[stream, buffer] : request->buffers()) {
			buffer->_d()->cancel();
			completeBuffer(request, buffer);
		}
		completeRequest(request);
	}
	LOG(GbosIpu6, Info) << data->id_ << ": stopped after " << data->frames_ << " frames";
}

int PipelineHandlerGbosIpu6::queueRequestDevice(Camera *camera, Request *request)
{
	cameraData(camera)->pending_.push_back(request);
	return 0;
}

bool PipelineHandlerGbosIpu6::match([[maybe_unused]] DeviceEnumerator *enumerator)
{
	if (created_)
		return false;
	created_ = true;

	bool any = false;
	for (const CameraInfo &info : kCameras) {
		if (!channelListening(info.id)) {
			LOG(GbosIpu6, Info) << info.id << ": no bridge on @" IPU6_CHANNEL_PREFIX << info.id
					    << "; camera not registered";
			continue;
		}
		auto data = std::make_unique<GbosIpu6CameraData>(this, info);
		std::set<Stream *> streams;
		for (Stream &stream : data->streams_)
			streams.insert(&stream);
		registerCamera(Camera::create(std::move(data), info.id, streams));
		LOG(GbosIpu6, Info) << info.id << ": registered (" << info.model << ")";
		any = true;
	}
	return any;
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerGbosIpu6)

} /* namespace libcamera */
