"""HTML for gbos86.com. Everything user-supplied goes through html.escape (e())."""
import html, os, re, time, urllib.parse
import laptops

e = html.escape


def _ver(f):
    try:
        return str(int(os.path.getmtime(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'static', f))))
    except OSError:
        return '0'


CSS_V, JS_V = _ver('site.css'), _ver('site.js')
URL_RE = re.compile(r'(https?://[^\s<>"\']+)')


def render_body(text):
    """Escape everything, then allow: ``` code ```, `code`, **bold**, '> ' quotes and bare http(s) links."""
    out = []
    for i, part in enumerate(text.split('```')):
        if i % 2 == 1:
            out.append('<pre><code>' + e(part.strip('\n')) + '</code></pre>')
            continue
        lines = []
        for line in part.split('\n'):
            s = e(line)
            s = re.sub(r'`([^`]+)`', r'<code>\1</code>', s)
            s = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', s)
            s = URL_RE.sub(lambda m: '<a href="%s" rel="nofollow ugc noopener" target="_blank">%s</a>' % (m.group(1), m.group(1)), s)
            if s.startswith('&gt; '):
                s = '<span class="q">' + s[5:] + '</span>'
            lines.append(s)
        out.append('<br>'.join(lines))
    return ''.join(out)


def ago(ts):
    d = int(time.time()) - ts
    if d < 60: return 'just now'
    if d < 3600: return '%d min ago' % (d // 60)
    if d < 86400: return '%d h ago' % (d // 3600)
    if d < 86400 * 30: return '%d d ago' % (d // 86400)
    return time.strftime('%d %b %Y', time.gmtime(ts))


def when(ts):
    return time.strftime('%d %b %Y, %H:%M UTC', time.gmtime(ts))


TURNSTILE_SITE = ''


def captcha():
    if not TURNSTILE_SITE:
        return ''
    return ('<div class="cf-turnstile" data-sitekey="%s" data-theme="auto"></div>'
            '<script src="https://challenges.cloudflare.com/turnstile/v0/api.js" async defer></script>' % e(TURNSTILE_SITE))


def csrf(req):
    return '<input type="hidden" name="csrf" value="%s">' % req.csrf


def hp():
    return '<div class="hp" aria-hidden="true"><label>Website <input name="website" tabindex="-1" autocomplete="off"></label></div>'


NAV = [('docs', 'Install &amp; Docs', '/docs'), ('hardware', 'Hardware', '/hardware'), ('download', 'Get it', '/download'),
       ('forum', 'Forum', '/forum'), ('faq', 'FAQ', '/faq'), ('about', 'About', '/about'), ('contact', 'Contact', '/contact')]


def layout(req, title, body, desc='', nav=''):
    full = 'gbOS86' if title.startswith('gbOS86') else title + ' | gbOS86'
    desc = desc or 'gbOS86: GooglebookOS for non-Google PCs. Patched, with hardware detection for Intel 6th-gen and newer laptops.'
    links = ''.join('<a href="%s"%s>%s</a>' % (u, ' class="on"' if k == nav else '', t) for k, t, u in NAV)
    if req.user:
        acct = ('<a class="user" href="/u/%s">%s</a>%s<form method="post" action="/logout" class="inline">%s<button class="link">Log out</button></form>'
                % (urllib.parse.quote(req.user['name']), e(req.user['name']), '<a href="/admin">Admin</a>' if req.user['is_admin'] else '', csrf(req)))
    else:
        acct = '<a href="/login">Log in</a><a class="btn small" href="/register">Join</a>'
    return '''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>%s</title><meta name="description" content="%s">
<meta property="og:title" content="%s"><meta property="og:description" content="%s"><meta property="og:type" content="website">
<meta property="og:image" content="https://gbos86.com/static/og.png"><meta name="twitter:card" content="summary_large_image">
<meta name="theme-color" content="#0b5fff"><link rel="icon" href="/static/favicon.png"><link rel="apple-touch-icon" href="/static/apple-touch-icon.png">
<link rel="stylesheet" href="/static/site.css?v=%s"></head><body>
<header class="top"><div class="wrap bar">
<a class="brand" href="/"><img src="/static/laptop.png" alt="" width="46" height="25"><span>gb<b>OS</b>86</span></a>
<button class="burger" aria-label="Menu" aria-expanded="false" data-burger>&#9776;</button>
<nav class="main" id="nav">%s</nav>
<div class="acct" id="acct">%s</div></div></header>
<main class="wrap">%s</main>
<footer><div class="wrap foot"><div><strong>gbOS86</strong> GooglebookOS for Non-Google PCs<br>
<span class="mut">Community project. Not affiliated with or endorsed by Google. Android and Chrome are trademarks of Google LLC.</span></div>
<div class="flinks"><a href="/docs/install">Install guide</a><a href="/hardware">Hardware</a><a href="/forum">Forum</a><a href="https://discord.gg/h9AD9fSnDT" rel="noopener" target="_blank">Discord</a><a href="https://github.com/linuxkid473/gbos-x86" rel="noopener" target="_blank">GitHub</a><a href="/faq">FAQ</a><a href="/contact">Contact</a></div></div></footer>
<script src="/static/site.js?v=%s" defer></script></body></html>''' % (e(full), e(desc), e(full), e(desc), CSS_V, links, acct, body.replace('<!--CAP-->', captcha()), JS_V)


WELCOME_POST = '''Welcome to the gbOS86 community.

gbOS86 runs Google's Googlebook OS on ordinary Intel laptops. It boots from a USB stick (live), installs to the internal SSD, or updates an existing install without touching your data.

**What works today:** Intel 6th gen and newer, Intel graphics, boot-time hardware detection for Wi-Fi, Bluetooth, sound, touchpad, brightness and display. Twenty machines have been tried so far (Dell Latitude 5300, Acer Aspire Lite AL15-52, HP Envy 13, ThinkPad T470 and Intel NUC8BEH work; others partly or not yet): https://gbos86.com/hardware. Your reports matter.

**How to help**
- Read the install guide: https://gbos86.com/docs/install
- Post your laptop and your `hwdetect.txt` in Hardware Compatibility
- Report bugs in Support with the logs from `gbos/logs/boot-N/`

Be kind, be specific, attach logs.'''


# ------------------------------------------------------------------ home
def home(req, cats, latest, news, st):
    cat_rows = ''.join('<li><a href="/forum/c/%s"><strong>%s</strong><span>%s</span></a><em>%d threads</em></li>' % (c['slug'], e(c['title']), e(c['descr']), c['nt']) for c in cats[1:7])
    lat = ''.join('<li><a href="/forum/t/%d">%s</a><span>%s &middot; %s &middot; %s</span></li>' % (t['id'], e(t['title']), e(t['ctitle']), e(t['author']), ago(t['last_post'])) for t in latest) \
        or '<li class="mut">No threads yet. Be the first.</li>'
    nw = ''.join('<li><a href="/forum/t/%d">%s</a><span>%s</span></li>' % (n['id'], e(n['title']), ago(n['created'])) for n in news)
    return '''
<section class="hero"><div class="hero-text">
<p class="eyebrow">GooglebookOS for Non-Google PCs</p>
<h1>Run Googlebook OS on the laptop you already own.</h1>
<p class="lead">gbOS86 is a patched, hardware-detecting build of Google's Googlebook OS for Intel 6th-gen and newer laptops. Boot it from a USB stick, or install it to the SSD.</p>
<div class="cta"><a class="btn" href="/docs/install">Install guide</a><a class="btn ghost" href="/hardware">Is my laptop supported?</a><a class="btn ghost" href="/forum">Community forum</a><a class="btn ghost" href="https://discord.gg/h9AD9fSnDT" rel="noopener" target="_blank">Join Discord</a><a class="btn ghost" href="https://github.com/linuxkid473/gbos-x86" rel="noopener" target="_blank">GitHub</a></div>
<ul class="chips"><li>SHA-NI / VNNI / GFNI / VAES patched</li><li>Auto-detects Wi-Fi, Bluetooth, sound, touchpad</li><li>Live USB, install or update</li></ul>
</div><div class="hero-art"><img src="/static/logo.png" alt="gbOS86 logo: a laptop running Googlebook OS" width="420" height="420"></div></section>

<section class="demo card"><div class="demo-text"><p class="eyebrow">Running on a Dell Latitude 5300</p><h2>See it running</h2>
<p>Googlebook OS running on a Dell Latitude 5300 business laptop (Intel i5-8365U): the desktop and taskbar, the Bluetooth and Wi-Fi quick settings, the touchpad and keyboard.</p>
<p class="mut">43 seconds, no sound, filmed on the laptop itself. Twenty machines have been tried so far, see the <a href="/hardware#tested">list</a>; <a href="/hardware">help us find out about yours</a>.</p></div>
<video controls preload="metadata" playsinline poster="/static/demo-poster.jpg" width="320" height="568"><source src="/static/demo.mp4" type="video/mp4">Your browser can't play this video. <a href="/static/demo.mp4">Download it</a>.</video></section>

<section class="grid3">
<div class="card"><h3>Patched for your CPU</h3><p>Google's OS expects instructions older chips lack. gbOS86 emulates them in the kernel and pre-patches the hot libraries, so boot and Play Store installs stay fast.</p></div>
<div class="card"><h3>Generic hardware support</h3><p>Every boot reads your devices and loads the right drivers: Intel, Realtek, MediaTek, Qualcomm and Broadcom Wi-Fi, Bluetooth, HDA and SOF audio, I2C and PS/2 touchpads, laptop hotkeys, backlight.</p></div>
<div class="card"><h3>Safe to try</h3><p>Run it live from USB first. Installing erases the SSD only after you type its serial number on the laptop, and Update keeps your data.</p></div>
</section>

<section class="cols"><div class="card"><h2>Latest activity</h2><ul class="list">%s</ul><p><a href="/forum">Open the forum &rarr;</a></p></div>
<div><div class="card"><h2>News</h2><ul class="list">%s</ul></div>
<div class="card"><h2>Community</h2><div class="stats"><div><b>%d</b><span>members</span></div><div><b>%d</b><span>threads</span></div><div><b>%d</b><span>posts</span></div></div></div></div></section>

<section class="card"><h2>Forums</h2><ul class="catlist">%s</ul></section>

<section class="notice"><strong>Tried so far:</strong> Dell Latitude 5300, Acer Aspire Lite AL15-52, Lenovo ThinkPad T470 and Intel NUC8BEH work; HP OmniBook 7 and Dell XPS 16 partly; Framework 13 Pro has a fix pending; HP 14-ne0013dx does not. See the <a href="/hardware#tested">full list</a>. Post your laptop in
<a href="/forum/c/hardware">Hardware Compatibility</a> and attach <code>hwdetect.txt</code>.</section>''' % (lat, nw, st['members'], st['threads'], st['posts'], cat_rows)


# ------------------------------------------------------------------ forum
def crumbs(*items):
    return '<nav class="crumbs">' + ' &rsaquo; '.join('<a href="%s">%s</a>' % (u, e(t)) if u else e(t) for t, u in items) + '</nav>'


def forum_index(req, cats):
    rows = []
    for c in cats:
        last = ''
        if c['lastt']:
            title, tid, ts = c['lastt'].split('\x01')
            last = '<a href="/forum/t/%s">%s</a><br><span class="mut">%s</span>' % (tid, e(title[:50]), ago(int(ts)))
        rows.append('<tr><td><a class="ct" href="/forum/c/%s">%s</a><div class="mut">%s</div></td><td class="num">%d</td><td class="num">%d</td><td class="lastc">%s</td></tr>'
                    % (c['slug'], e(c['title']), e(c['descr']), c['nt'], c['np'], last or '<span class="mut">no posts</span>'))
    return ('<div class="pagehead"><h1>Forum</h1><form class="search" action="/search"><input name="q" placeholder="Search threads" aria-label="Search"><button class="btn small">Search</button></form></div>'
            '<div class="card flush"><table class="forum"><thead><tr><th>Forum</th><th class="num">Threads</th><th class="num">Posts</th><th>Last post</th></tr></thead><tbody>%s</tbody></table></div>' % ''.join(rows))


def pager(base, pg, pages_):
    if pages_ <= 1: return ''
    sep = '&' if '?' in base else '?'
    return '<div class="pager">' + ''.join('<a href="%s%sp=%d"%s>%d</a>' % (base, sep, i, ' class="on"' if i == pg else '', i) for i in range(1, pages_ + 1)) + '</div>'


def category(req, cat, ths, pg, pages_):
    rows = ''.join('<tr><td>%s<a class="ct" href="/forum/t/%d">%s</a><div class="mut">by <a href="/u/%s">%s</a> &middot; %s</div></td><td class="num">%d</td><td class="num">%d</td><td class="lastc">%s<br><span class="mut">%s</span></td></tr>'
                   % ('<span class="tag">Pinned</span> ' if t['pinned'] else '', t['id'], e(t['title']), urllib.parse.quote(t['author']), e(t['author']), ago(t['created']),
                      t['replies'], t['views'], e(t['lastby'] or ''), ago(t['last_post'])) for t in ths) or '<tr><td colspan="4" class="mut">No threads yet.</td></tr>'
    can = req.user and (not cat['staff_only'] or req.user['is_admin'])
    btn = '<a class="btn" href="/forum/c/%s/new">New thread</a>' % cat['slug'] if can else ('<a class="btn" href="/login?next=/forum/c/%s/new">Log in to post</a>' % cat['slug'] if not req.user and not cat['staff_only'] else '')
    return (crumbs(('Forum', '/forum'), (cat['title'], None)) + '<div class="pagehead"><div><h1>%s</h1><p class="mut">%s</p></div>%s</div>' % (e(cat['title']), e(cat['descr']), btn)
            + '<div class="card flush"><table class="forum"><thead><tr><th>Thread</th><th class="num">Replies</th><th class="num">Views</th><th>Last</th></tr></thead><tbody>%s</tbody></table></div>' % rows
            + pager('/forum/c/' + cat['slug'], pg, pages_))


def thread(req, t, ps, pg, pages_):
    adm = req.user and req.user['is_admin']
    first_id = ps[0]['id'] if ps else 0
    out = []
    for p in ps:
        tools = ''
        if adm and p['id'] != first_id:
            tools = '<form method="post" action="/forum/mod/delpost/%d" class="inline">%s<button class="link danger">Delete</button></form>' % (p['id'], csrf(req))
        out.append('<article class="post" id="p%d"><aside><a class="pn" href="/u/%s">%s</a>%s<span class="mut">%d posts</span>%s</aside><div class="pb"><div class="pm"><a href="#p%d">%s</a>%s</div><div class="txt">%s</div></div></article>'
                   % (p['id'], urllib.parse.quote(p['author']), e(p['author']), '<span class="tag">Staff</span>' if p['admin'] else '', p['pcount'],
                      ('<span class="mut">%s</span>' % e(p['laptop'])) if p['laptop'] else '', p['id'], when(p['created']), tools, render_body(p['body'])))
    modbar = ''
    if adm:
        def b(a, label): return '<form method="post" action="/forum/mod/%s/%d" class="inline">%s<button class="btn small ghost">%s</button></form>' % (a, t['id'], csrf(req), label)
        modbar = '<div class="modbar">%s%s%s</div>' % (b('unpin' if t['pinned'] else 'pin', 'Unpin' if t['pinned'] else 'Pin'), b('unlock' if t['locked'] else 'lock', 'Unlock' if t['locked'] else 'Lock'), b('delthread', 'Delete thread'))
    if t['locked'] and not adm:
        reply = '<div class="notice">This thread is locked.</div>'
    elif req.user and req.user['created'] > time.time() - 900 and not req.user['is_admin']:
        reply = '<div class="notice">New accounts can post after 15 minutes. Come back soon, or read the <a href="/docs/install">install guide</a> meanwhile.</div>'
    elif req.user:
        reply = ('<form class="card" method="post" action="/forum/t/%d/reply"><h3>Reply</h3>%s<textarea name="body" rows="7" maxlength="20000" required placeholder="Write your reply. **bold**, `code`, ``` blocks ```, &gt; quote"></textarea><button class="btn">Post reply</button></form>' % (t['id'], csrf(req)))
    else:
        reply = '<div class="notice"><a href="/login?next=/forum/t/%d">Log in</a> or <a href="/register">join</a> to reply.</div>' % t['id']
    return (crumbs(('Forum', '/forum'), (t['ctitle'], '/forum/c/' + t['cslug']), (t['title'], None))
            + '<div class="pagehead"><h1>%s%s</h1>%s</div>' % ('<span class="tag">Locked</span> ' if t['locked'] else '', e(t['title']), modbar)
            + pager('/forum/t/%d' % t['id'], pg, pages_) + ''.join(out) + pager('/forum/t/%d' % t['id'], pg, pages_) + reply)


def new_thread(req, cat, title, body, msg):
    return (crumbs(('Forum', '/forum'), (cat['title'], '/forum/c/' + cat['slug']), ('New thread', None))
            + '<form class="card narrow wide" method="post"><h1>New thread</h1><p class="mut">in %s</p>%s%s<label>Title<input name="title" value="%s" maxlength="120" required></label>'
              '<label>Message<textarea name="body" rows="12" maxlength="20000" required placeholder="For hardware reports: laptop model, CPU, what works, what does not, and paste hwdetect.txt in a ``` block.">%s</textarea></label>'
              '<p class="mut">Formatting: **bold**, `code`, ``` code blocks ```, &gt; quotes. Links are made clickable.</p><button class="btn">Post thread</button></form>'
            % (e(cat['title']), ('<p class="err">%s</p>' % e(msg)) if msg else '', csrf(req), e(title), e(body)))


def register(req, msg, name, email):
    return ('<form class="card narrow" method="post" action="/register"><h1>Join gbOS86</h1>%s%s%s<label>Username<input name="name" value="%s" maxlength="24" required autocomplete="username"></label>'
            '<label>Email <span class="mut">(required, never shown)</span><input name="email" type="email" value="%s" maxlength="120" required autocomplete="email"></label>'
            '<label>Password<input name="password" type="password" minlength="10" required autocomplete="new-password"></label>'
            '<label>Repeat password<input name="password2" type="password" minlength="10" required autocomplete="new-password"></label>'
            '<!--CAP--><button class="btn">Create account</button><p class="mut">Already a member? <a href="/login">Log in</a>. Be kind: see the <a href="/faq#rules">rules</a>.</p></form>'
            % (('<p class="err">%s</p>' % e(msg)) if msg else '', csrf(req), hp(), e(name), e(email)))


def login(req, msg, nxt):
    return ('<form class="card narrow" method="post" action="/login"><h1>Log in</h1>%s%s<input type="hidden" name="next" value="%s">'
            '<label>Username<input name="name" maxlength="24" required autocomplete="username"></label>'
            '<label>Password<input name="password" type="password" required autocomplete="current-password"></label>'
            '<!--CAP--><button class="btn">Log in</button><p class="mut">New here? <a href="/register">Create an account</a>.</p></form>'
            % (('<p class="err">%s</p>' % e(msg)) if msg else '', csrf(req), e(nxt)))


def profile(req, u, ths, cnt):
    rows = ''.join('<li><a href="/forum/t/%d">%s</a><span>%s &middot; %s</span></li>' % (t['id'], e(t['title']), e(t['ctitle']), ago(t['created'])) for t in ths) or '<li class="mut">No threads.</li>'
    edit = '<a class="btn small ghost" href="/settings">Edit profile</a>' if req.user and req.user['id'] == u['id'] else ''
    return ('<div class="card"><div class="pagehead"><h1>%s%s</h1>%s</div><p>%s</p><p class="mut">Member since %s &middot; %d posts%s</p></div><div class="card"><h2>Threads</h2><ul class="list">%s</ul></div>'
            % (e(u['name']), ' <span class="tag">Staff</span>' if u['is_admin'] else '', edit, e(u['bio']) or '<span class="mut">No bio.</span>', time.strftime('%b %Y', time.gmtime(u['created'])), cnt,
               (' &middot; Laptop: ' + e(u['laptop'])) if u['laptop'] else '', rows))


def settings(req, msg):
    return ('<form class="card narrow" method="post" action="/settings"><h1>Profile</h1>%s%s<label>Your laptop <span class="mut">(shown under your posts)</span><input name="laptop" value="%s" maxlength="100" placeholder="Dell Latitude 5300, i5-8365U"></label>'
            '<label>About you<textarea name="bio" rows="4" maxlength="500">%s</textarea></label><button class="btn">Save</button></form>'
            % (('<p class="err">%s</p>' % e(msg)) if msg else '', csrf(req), e(req.user['laptop']), e(req.user['bio'])))


def search(req, q, res):
    rows = ''.join('<li><a href="/forum/t/%d">%s</a><span>%s &middot; %s</span></li>' % (r['id'], e(r['title']), e(r['ctitle']), e(r['author'])) for r in res)
    return ('<div class="pagehead"><h1>Search</h1></div><form class="search big" action="/search"><input name="q" value="%s" placeholder="Search threads and posts" autofocus><button class="btn">Search</button></form>%s'
            % (e(q), ('<div class="card"><ul class="list">%s</ul></div>' % rows) if res else ('<p class="mut">No results for "%s".</p>' % e(q) if q else '')))


def contact(req, f, msg, ok):
    if ok:
        return '<div class="card narrow"><h1>Message sent</h1><p>Thanks. We read every message and will reply by email if needed.</p><p><a href="/">Back to the front page</a></p></div>'
    return ('<form class="card narrow wide" method="post" action="/contact"><h1>Contact</h1><p class="mut">Questions, press, bug reports, hardware you would like supported. For help with a problem, the <a href="/forum/c/support">support forum</a> is faster.</p>%s%s%s'
            '<label>Name<input name="name" value="%s" maxlength="80" required></label><label>Email<input name="email" type="email" value="%s" maxlength="120" required></label>'
            '<label>Subject<input name="subject" value="%s" maxlength="120" required></label><label>Message<textarea name="body" rows="8" maxlength="5000" required>%s</textarea></label>'
            '<!--CAP--><button class="btn">Send message</button></form>'
            % (('<p class="err">%s</p>' % e(msg)) if msg else '', csrf(req), hp(), e(f['name']), e(f['email']), e(f['subject']), e(f['body'])))


def admin(req, msgs, users):
    m = ''.join('<article class="card%s"><div class="pm"><strong>%s</strong> &lt;%s&gt; &middot; %s<form method="post" action="/admin" class="inline">%s<input type="hidden" name="handle" value="%d"><button class="link">%s</button></form></div><h3>%s</h3><div class="txt">%s</div></article>'
                % (' done' if x['handled'] else '', e(x['name']), e(x['email']), when(x['created']), csrf(req), x['id'], 'Mark open' if x['handled'] else 'Mark handled', e(x['subject']), render_body(x['body'])) for x in msgs) or '<p class="mut">No messages.</p>'
    u = ''.join('<li><span>%s%s</span><span>%s</span></li>' % (e(x['name']), ' (staff)' if x['is_admin'] else '', when(x['created'])) for x in users)
    return '<div class="pagehead"><h1>Admin</h1></div><h2>Contact messages</h2>%s<h2>Newest members</h2><div class="card"><ul class="list">%s</ul></div>' % (m, u)


# ------------------------------------------------------------------ content pages
def guide(req):
    return '''<div class="doc"><h1>Install guide</h1>
<p class="lead">From a blank USB stick to a running Googlebook OS. Read it through once before you start: step 4 can erase your laptop's drive.</p>
<div class="toc card"><strong>On this page</strong><a href="#need">What you need</a><a href="#flash">1. Flash the stick</a><a href="#boot">2. Boot it</a><a href="#live">3. Try it live</a><a href="#install">4. Install</a><a href="#update">5. Update later</a><a href="#trouble">If something is wrong</a></div>

<h2 id="need">What you need</h2>
<ul><li>A laptop with an <strong>Intel 6th-gen (Skylake) or newer CPU with AVX</strong> (many Celeron, Pentium and Atom chips have none and cannot run it) and Intel graphics. See <a href="/hardware">Hardware</a>.</li>
<li>A <strong>USB stick of 8 GB or more</strong> (it will be erased).</li>
<li>The gbOS86 image (<a href="/download">how to get it</a>) and a flashing tool: balenaEtcher, Rufus, or <code>dd</code>.</li>
<li>For installing: a laptop with an <strong>internal NVMe SSD</strong> that you are happy to erase, and a backup of anything on it. Live mode touches nothing and works without NVMe.</li></ul>

<h2 id="flash">1. Flash the stick</h2>
<p>Write the whole <code>.img</code> file to the USB stick (not copy it as a file). With balenaEtcher: Flash from file, select the image, select the stick, Flash. On macOS or Linux:</p>
<pre><code>sudo dd if=gbOS86.img of=/dev/rdiskN bs=4m   # macOS (check N with diskutil list)
sudo dd if=gbOS86.img of=/dev/sdX bs=4M status=progress   # Linux</code></pre>

<h2 id="boot">2. Boot it</h2>
<ol><li>Plug the stick into the laptop and power on.</li><li>Press the boot-menu key (F12 on Dell and Lenovo, F9 on HP, Esc or F8 on Asus, F12 on Acer) and choose the USB stick (UEFI).</li>
<li>In firmware settings, turn <strong>Secure Boot off</strong> if the stick will not start.</li></ol>
<p>The gbOS86 menu appears with a 10 second timer:</p>
<table class="plain"><tr><td><b>1</b></td><td>Live from USB. Nothing is saved.</td></tr><tr><td><b>2</b></td><td>Install or update on the internal SSD.</td></tr><tr><td><b>3</b></td><td>Boot the installed system (default when one exists).</td></tr></table>

<h2 id="live">3. Try it live</h2>
<p>Choose <b>1</b>. The first boot takes a while because some CPU instructions are emulated. At the first screen your Wi-Fi, Bluetooth, sound, touchpad and brightness should work; if not, see <a href="#trouble">troubleshooting</a>.</p>

<h2 id="install">4. Install to the internal SSD</h2>
<div class="notice warn"><strong>This erases the internal drive.</strong> The installer will show the drive's model and serial number and make you type that serial number on the laptop before it erases anything. There is no way to skip it.</div>
<ol><li>Boot the stick and choose <b>2</b>.</li><li>The installer shows the NVMe drive's model, size and serial number and says everything on it will be erased. To continue you type the serial number exactly as shown; anything else cancels.</li>
<li>It partitions the drive, copies the system and data partitions, verifies every file with a checksum, and tells you when to remove the stick.</li><li>Remove the stick and boot from the internal drive.</li></ol>

<h2 id="update">5. Update later</h2>
<p>Boot a newer stick and choose <b>2</b>. When it finds an existing install it offers <b>1) Update</b> (keeps apps, accounts and settings) or <b>2) Reinstall</b> (erases). Choose 1, wait for "Update done", then remove the stick.</p>

<h2 id="trouble">If something is wrong</h2>
<ul><li>Black screen or reboot loop on the stick: try another USB port (a USB 2.0 port is often more reliable), and check Secure Boot is off.</li>
<li>Edit <code>gbos/gbos.cfg</code> on the stick from any computer: <code>HWDETECT=0</code> turns off auto-detection; <code>LOAD_I915=0</code> avoids the graphics driver if the screen freezes right after it loads; <code>RESOLUTION</code> and <code>DENSITY</code> change the UI size.</li>
<li>Every boot writes logs to <code>gbos/logs/boot-N/</code> on the stick. <code>hwdetect.txt</code> lists your devices, which ones got a driver, and any missing firmware. Post it in the <a href="/forum/c/support">support forum</a>.</li></ul></div>'''


def hardware(req):
    return '''<div class="doc"><h1>Hardware</h1>
<p class="lead">gbOS86 targets <strong>Intel 6th-gen (Skylake) and newer</strong> laptops with Intel graphics. At every boot it reads your devices and loads matching drivers.</p>
<div class="notice"><strong>Be realistic:</strong> twenty machines have been tried, eight of them fully working. Newer chips (Core Ultra, Wi-Fi 7) are the weak spot. Reports from your laptop make this table real: <a href="/forum/c/hardware">post yours</a>.</div>
<h2 id="tested">Laptops people have tried</h2>
''' + laptops.table_html() + '''
<h2>What the drivers cover</h2>
<table class="spec"><thead><tr><th>Area</th><th>Supported</th></tr></thead><tbody>
<tr><td>CPU</td><td>Intel Core, 6th gen and newer, with AVX (check <code>grep avx /proc/cpuinfo</code> on any Linux; many Celeron/Pentium/Atom chips lack it and boot stops with a message). Missing instructions (SHA-NI, AVX-VNNI, GFNI, VAES, VPCLMULQDQ, AVX-IFMA) are emulated in the kernel; the hottest code is pre-patched. Faster on newer CPUs that have them natively.</td></tr>
<tr><td>Graphics, display</td><td>Intel i915 from Skylake to Raptor Lake, eDP panels, HDMI/DP. Newer chips use xe, which needs its own firmware (a Framework 13 Pro stayed black without it); see the list above. Colour fixes apply to i915 only. AMD Radeon: experimental, drawn by the CPU (software rendering), slow, untested on hardware.</td></tr>
<tr><td>Brightness</td><td>Backlight class and ACPI video, plus laptop hotkey drivers (Dell, HP, ThinkPad, IdeaPad, Asus, Acer, Samsung, MSI, Sony, Fujitsu, LG, Intel HID).</td></tr>
<tr><td>Wi-Fi</td><td>Intel 7260/7265, 8265, 9000-series, AX200/201/210/211, BE200 (iwldvm and iwlmvm). Realtek rtw88 and rtw89. MediaTek mt7921/22/25. Qualcomm ath10k and ath11k. Broadcom brcmfmac.</td></tr>
<tr><td>Bluetooth</td><td>USB Bluetooth from Intel, Realtek, MediaTek, Qualcomm and Broadcom, plus Intel PCIe Bluetooth.</td></tr>
<tr><td>Sound</td><td>Legacy HDA with the common codecs (Realtek, Conexant, Cirrus, IDT/SigmaTel, VIA), or SOF for newer machines with digital mics or SoundWire. The kernel picks per machine. SOF is best effort.</td></tr>
<tr><td>Touchpad, keyboard</td><td>Intel LPSS I2C-HID touchpads (most modern laptops), Intel sensor hub, PS/2 (Synaptics, ALPS, Elan), Synaptics RMI4.</td></tr>
<tr><td>Storage</td><td><strong>Installing needs an internal NVMe SSD</strong> (the installer only targets NVMe). SATA-only laptops can still run live from USB. SD card readers (Realtek, SDHCI) are supported.</td></tr>
<tr><td>Other</td><td>USB webcams (UVC), Ethernet (r8169, e1000e, igc, common USB adapters).</td></tr></tbody></table>
<h2>Not supported</h2>
<p>Suspend and resume, fingerprint readers, IPU6 MIPI cameras, NVIDIA and AMD discrete GPUs, AMD CPUs, ARM (AMD integrated graphics is experimental, see above). Some security-chip features (Google GSC, Trusty) do not exist on other laptops, so they are replaced in software.</p>
<h2>The ideal laptop</h2>
<p>A 2018 to 2022 business laptop: <strong>Intel 8th to 11th gen, Intel UHD or Iris Xe graphics, 8 GB or more of RAM, NVMe SSD, 1080p panel, Intel Wi-Fi and Bluetooth, Intel HDA audio</strong>. Dell Latitude, Lenovo ThinkPad and HP EliteBook of that period are the most predictable.</p>
<h2>Reporting your laptop</h2>
<p>Boot the stick once, shut down, put it in another computer and open <code>gbos/logs/boot-N/hwdetect.txt</code>. Post it in <a href="/forum/c/hardware">Hardware Compatibility</a> with the model name and what works.</p></div>'''


def download(req):
    return '''<div class="doc"><h1>Get gbOS86</h1>
<div class="notice warn"><strong>Why there is no download button.</strong> The image contains Google's OS and Google libraries that we patch. Redistributing those is not ours to do, so gbOS86 ships as <em>tools</em>: you build the image yourself from Google's public recovery image. Everything we wrote is in the build tools.</div>
<h2>Build it in one command</h2>
<pre><code>git clone https://github.com/linuxkid473/gbos-x86 &amp;&amp; cd gbos-x86
./build.sh</code></pre>
<p>The script asks before downloading anything, then: fetches Google's public recovery image (7.7 GB) and extracts only the six partitions gbOS86 needs, gets a small busybox, the kernel modules and the firmware, and assembles <code>dist/gbOS86-DATE.img</code> (about 6.7 GB). Flash it to a USB stick of 8 GB or more (balenaEtcher, Rufus or <code>dd</code>), boot it, and follow the <a href="/docs/install">install guide</a>. <code>./build.sh --dry-run</code> shows what it would download.</p>
<p>You need a Mac or Linux machine with Docker (running), Python 3 and about 35 GB free. The tools, the kernel patches and the full build notes are in the <a href="https://github.com/linuxkid473/gbos-x86" rel="noopener" target="_blank">open-source repository on GitHub</a>. It does not contain Google's OS images, which you download yourself; it does include the small set of patched Google libraries the build needs. Ask in the <a href="/forum/c/downloads">Downloads &amp; Builds</a> forum or on Discord if you get stuck.</p>
<h2>What is inside</h2>
<table class="spec"><tbody><tr><td>Kernel</td><td>Linux 6.18 (Android common), CPU-instruction emulation, Intel i915 with colour fixes, about 400 driver modules.</td></tr>
<tr><td>Patched</td><td>libz and libcrypto without SHA-NI, bound before the first partition mounts so boot is not slowed by trapped instructions.</td></tr>
<tr><td>Boot menu</td><td>Live, install or update, boot installed. Install needs the drive serial typed on the laptop.</td></tr>
<tr><td>Detection</td><td>Boot-time device probing and driver loading, with a per-boot <code>hwdetect.txt</code> report.</td></tr></tbody></table></div>'''


def faq(req):
    qa = [('Is this official?', 'No. gbOS86 is an independent community project and is not affiliated with or endorsed by Google.'),
          ('Will it work on my laptop?', 'If it has an Intel 6th-gen or newer CPU and Intel graphics, probably yes; see <a href="/hardware">Hardware</a>. Twenty machines have been tried (eight work fully; newer Core Ultra machines are the weak spot), see the <a href="/hardware#tested">list</a>. Try live mode first.'),
          ('Can I break my laptop?', 'Live mode writes nothing to the internal drive. Installing erases the internal drive, and only after you type its serial number on the laptop. Back up first.'),
          ('Why is the first boot slow?', 'Google\'s OS uses CPU instructions older chips lack. The kernel emulates them, which costs time. gbOS86 patches the worst offenders and loads those patches before they can slow boot.'),
          ('Does Wi-Fi / Bluetooth / sound work?', 'On the Dell Latitude 5300 and the Acer Aspire Lite AL15-52, yes (the Acer needed firmware fixes that are now in the build). On Core Ultra machines Wi-Fi is the open problem. See the <a href="/hardware#tested">tested list</a> and send us your <code>hwdetect.txt</code>.'),
          ('Does suspend work?', 'Not yet.'),
          ('Why can I not download the image?', 'It contains Google\'s OS and patched Google libraries. See <a href="/download">Get gbOS86</a>.'),
          ('Does it work on AMD or ARM?', 'Not supported. Intel x86 only. AMD Radeon graphics has an experimental software-rendering mode (slow, untested on hardware); AMD CPUs and ARM are not supported.'),
          ('How do I update?', 'Boot a newer stick, choose 2 (install or update), then Update. Your data is kept.'), ('Can I install on a SATA SSD?', 'Not yet. The installer only targets an internal NVMe drive. You can still run live from the USB stick.')]
    return '<div class="doc"><h1>FAQ</h1>' + ''.join('<details class="card"><summary>%s</summary><p>%s</p></details>' % (q, a) for q, a in qa) + '''
<h2 id="rules">Forum rules</h2><ol><li>Be respectful. No harassment or abuse.</li><li>Attach logs and details when asking for help.</li><li>No spam, ads or off-topic promotion.</li><li>Staff may edit or remove posts and ban accounts that break these rules.</li></ol></div>'''


def about(req):
    return '''<div class="doc"><h1>About gbOS86</h1>
<p class="lead">GooglebookOS for Non-Google PCs.</p>
<p>Googlebook OS is Google's laptop operating system, built on Android. It is made for Google's own hardware, with its own security chip, firmware and kernel. gbOS86 makes it start and run on ordinary Intel laptops by replacing the parts that depend on that hardware.</p>
<h2>How it works</h2>
<ul><li><strong>A different kernel.</strong> Linux 6.18 built for generic Intel laptops, with an emulator that handles CPU instructions older chips lack.</li>
<li><strong>Binary patches.</strong> The libraries that execute those instructions most (zlib checksums and SHA-256 in libcrypto) are replaced by versions that do not need them, but only when the original file matches an exact checksum.</li>
<li><strong>Software stand-ins</strong> for Google-only hardware (security chip, Trusty VM): a software Gatekeeper, hidden or replaced services.</li>
<li><strong>Hardware detection.</strong> At boot it reads every device ID, loads the matching driver and firmware, and writes a report.</li></ul>
<h2>Status</h2>
<p>Boots to the desktop, sign-in, Play Store, persistent data on the internal drive, and updates from a USB stick. Fully working on a Dell Latitude 5300, an Acer Aspire Lite AL15-52 and a ThinkPad T470; partly on newer machines (see the <a href="/hardware#tested">tested list</a>). Broad Intel support is new and needs your reports.</p>
<h2>Contact</h2><p>Source code: <a href="https://github.com/linuxkid473/gbos-x86" rel="noopener" target="_blank">github.com/linuxkid473/gbos-x86</a>. Questions: the <a href="/forum">forum</a>, <a href="https://discord.gg/h9AD9fSnDT" rel="noopener" target="_blank">Discord</a> or the <a href="/contact">contact form</a>.</p></div>'''


# ------------------------------------------------------------------ docs section
def docs_index(req):
    import docs
    cards = ''.join('<a class="card doccard" href="/docs/%s"><h3>%s</h3><p class="mut">%s</p></a>' % (s, e(t), e(sm)) for s, t, sm, _ in docs.DOCS)
    return ('<div class="pagehead"><div><h1>Install &amp; Docs</h1><p class="lead">Everything about gbOS86: installing it, how it works, and how to fix it when something is wrong.</p></div></div>'
            '<div class="grid3 docgrid">%s</div>' % cards)


def docs_page(req, slug):
    """(title, html) for one docs page, or None."""
    import docs
    for s, t, sm, fn in docs.DOCS:
        if s == slug:
            body = guide(req) if slug == 'install' else '<div class="doc">' + fn() + '</div>'
            side = ''.join('<a href="/docs/%s"%s>%s</a>' % (x, ' class="on"' if x == slug else '', e(xt)) for x, xt, _, _ in docs.DOCS)
            nxt = [i for i, d in enumerate(docs.DOCS) if d[0] == slug][0]
            pager_ = ''
            if nxt + 1 < len(docs.DOCS):
                pager_ = '<div class="docnext"><a href="/docs/%s">Next: %s &rarr;</a></div>' % (docs.DOCS[nxt + 1][0], e(docs.DOCS[nxt + 1][1]))
            return t, ('<div class="docs"><nav class="docnav" aria-label="Docs"><a class="all" href="/docs">All docs</a>%s</nav><div class="doccol">%s%s'
                       '<p class="mut docfoot">Something wrong or missing? <a href="/contact">Tell us</a> or ask in the <a href="/forum">forum</a>.</p></div></div>' % (side, body, pager_))
    return None
