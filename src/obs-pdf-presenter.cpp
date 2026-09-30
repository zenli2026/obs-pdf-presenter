/*
 * obs-pdf-presenter —— OBS 原生「演示文稿播放器」插件
 * 在 OBS 来源列表直接出现「演示文稿播放器」，选择 PDF 文件即可播放，
 * 支持快捷键翻页、跳页、自动播放。PDF 用 PDFium 渲染。
 * PPT/PPTX 请先用 PowerPoint 另存为 PDF 后再加载。
 *
 * 本项目按 GNU GPL v2 协议开源。
 */
#include <obs-module.h>
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>

#define PLUGIN_NAME    "obs-pdf-presenter"
#define PLUGIN_VERSION "1.0.0"

/* 悬浮控制窗消息/控件 ID */
#define WM_CTRL_REFRESH (WM_USER + 10)
#define ID_CTRL_PREV 1001
#define ID_CTRL_NEXT 1002
#define ID_CTRL_CLOSE 1003

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "zh-CN")

/* ============================= 运行时定位的资源 ============================= */
static std::string g_module_dir;   // 插件 dll 所在目录（放 pdfium.dll）

/* ============================= PDFium 动态绑定 ============================= */
typedef void  *FPDF_DOCUMENT;
typedef void  *FPDF_PAGE;
typedef void  *FPDF_BITMAP;
typedef unsigned int FPDF_DWORD;
typedef const char   *FPC_BYTESTRING;

static HMODULE g_pdfium = nullptr;
static void (*pFPDF_InitLibrary)(void);
static FPDF_DOCUMENT (*pFPDF_LoadDocument)(FPC_BYTESTRING, FPC_BYTESTRING);
static int (*pFPDF_GetPageCount)(FPDF_DOCUMENT);
static FPDF_PAGE (*pFPDF_LoadPage)(FPDF_DOCUMENT, int);
static void (*pFPDF_ClosePage)(FPDF_PAGE);
static void (*pFPDF_CloseDocument)(FPDF_DOCUMENT);
static float (*pFPDF_GetPageWidth)(FPDF_PAGE);
static float (*pFPDF_GetPageHeight)(FPDF_PAGE);
static int (*pFPDF_GetPageSizeByIndex)(FPDF_DOCUMENT, int, double *, double *);
static FPDF_BITMAP (*pFPDFBitmap_Create)(int, int, int);
static void (*pFPDFBitmap_FillRect)(FPDF_BITMAP, int, int, int, int, FPDF_DWORD);
static void *(*pFPDFBitmap_GetBuffer)(FPDF_BITMAP);
static void (*pFPDFBitmap_Destroy)(FPDF_BITMAP);
static void (*pFPDF_RenderPageBitmap)(FPDF_BITMAP, FPDF_PAGE, int, int, int,
				      int, int, int);
static void (*pFPDF_DestroyLibrary)(void);

static bool pdfium_ok()
{
	return g_pdfium && pFPDF_InitLibrary && pFPDF_LoadDocument &&
	       pFPDF_GetPageCount && pFPDF_LoadPage && pFPDF_ClosePage &&
	       pFPDF_CloseDocument && pFPDF_GetPageWidth && pFPDF_GetPageHeight &&
	       pFPDF_GetPageSizeByIndex &&
	       pFPDFBitmap_Create && pFPDFBitmap_FillRect && pFPDFBitmap_GetBuffer &&
	       pFPDFBitmap_Destroy && pFPDF_RenderPageBitmap;
}

/* ============================= 工具函数 ============================= */
static std::wstring utf8_to_wide(const std::string &utf8)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (n <= 0)
		return L"";
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
	return w;
}

static std::string wide_to_utf8(const std::wstring &w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
				    nullptr, nullptr);
	if (n <= 0)
		return "";
	std::string s(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
	return s;
}

static void load_pdfium()
{
	if (g_pdfium)
		return;
	std::string dll = g_module_dir + "\\pdfium.dll";
	g_pdfium = LoadLibraryW(utf8_to_wide(dll).c_str());
	if (!g_pdfium) {
		blog(LOG_WARNING, "[pdf-presenter] 未找到 pdfium.dll，PDF/PPT 无法渲染");
		return;
	}
#define LOAD_PFN(name, sym)                                                    \
	do {                                                                   \
		*(void **)&(sym) = (void *)GetProcAddress(g_pdfium, name);    \
	} while (0)
	LOAD_PFN("FPDF_InitLibrary", pFPDF_InitLibrary);
	LOAD_PFN("FPDF_LoadDocument", pFPDF_LoadDocument);
	LOAD_PFN("FPDF_GetPageCount", pFPDF_GetPageCount);
	LOAD_PFN("FPDF_LoadPage", pFPDF_LoadPage);
	LOAD_PFN("FPDF_ClosePage", pFPDF_ClosePage);
	LOAD_PFN("FPDF_CloseDocument", pFPDF_CloseDocument);
	LOAD_PFN("FPDF_GetPageWidth", pFPDF_GetPageWidth);
	LOAD_PFN("FPDF_GetPageHeight", pFPDF_GetPageHeight);
	LOAD_PFN("FPDF_GetPageSizeByIndex", pFPDF_GetPageSizeByIndex);
	LOAD_PFN("FPDFBitmap_Create", pFPDFBitmap_Create);
	LOAD_PFN("FPDFBitmap_FillRect", pFPDFBitmap_FillRect);
	LOAD_PFN("FPDFBitmap_GetBuffer", pFPDFBitmap_GetBuffer);
	LOAD_PFN("FPDFBitmap_Destroy", pFPDFBitmap_Destroy);
	LOAD_PFN("FPDF_RenderPageBitmap", pFPDF_RenderPageBitmap);
	LOAD_PFN("FPDF_DestroyLibrary", pFPDF_DestroyLibrary);
#undef LOAD_PFN
	if (pdfium_ok())
		pFPDF_InitLibrary();
}

/* ============================= 源结构 ============================= */

static std::string lowercase(const std::string &s)
{
	std::string r = s;
	std::transform(r.begin(), r.end(), r.begin(),
		       [](unsigned char c) { return (char)std::tolower(c); });
	return r;
}

/* 取扩展名（小写，不含点） */
static std::string get_ext(const std::string &path)
{
	size_t p = path.find_last_of('.');
	if (p == std::string::npos)
		return "";
	return lowercase(path.substr(p + 1));
}

/* 把路径里的 '/' 统一成 '\\'，避免 PowerShell / PowerPoint 无法解析 */
static std::string normalize_path(std::string s)
{
	for (auto &c : s)
		if (c == '/')
			c = '\\';
	return s;
}

/* 文件是否存在 */
static bool file_exists(const std::wstring &path)
{
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/* ============================= 源结构 ============================= */
#define CANVAS_W 1920
#define CANVAS_H 1080

struct pdf_source;

/* 悬浮控制窗（独立线程、始终置顶）：显示页数 + 上一页/下一页按钮 */
struct ctrl_window {
	HWND hwnd = nullptr;
	HWND label = nullptr;
	std::thread thd;
	pdf_source *src = nullptr;
	bool active = false;
	int page = 0;
	int total = 0;
};

struct pdf_source {
	obs_source_t *source;
	obs_data_t *settings;

	std::string srcFile;   // 用户选择的原始文件
	std::string pdfPath;   // 实际被渲染的 pdf（PPT 转换后）
	FPDF_DOCUMENT doc = nullptr;
	int pageCount = 0;
	int curPage = 0;

	gs_texture_t *tex = nullptr;
	int texW = 0, texH = 0;              // 纹理当前尺寸（渲染线程管理）
	int canvasW = CANVAS_W, canvasH = CANVAS_H; // 画布实际尺寸（随 PDF 页面比例）
	std::vector<uint8_t> canvas;         // canvasW x canvasH BGRA
	bool canvasDirty = false;            // 画布已更新，等待渲染线程上传纹理

	ctrl_window *cw = nullptr;           // 悬浮控制窗（可空）

	bool autoplay = false;
	int interval = 5;
	float acc = 0;   // 自动播放计时（秒）
};

static const char *get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Name");
}

static void render_page(struct pdf_source *s, int page)
{
	if (!s->doc || page < 1 || page > s->pageCount)
		return;

	/* 用 GetPageSizeByIndex 取页面尺寸（double 出参，稳定可靠） */
	double pw = 0, ph = 0;
	if (!pFPDF_GetPageSizeByIndex(s->doc, page - 1, &pw, &ph)) {
		blog(LOG_WARNING, "[pdf-presenter] GetPageSizeByIndex 失败 (page %d)", page);
		return;
	}
	if (pw <= 0 || ph <= 0) {
		blog(LOG_WARNING, "[pdf-presenter] 页面尺寸无效 (page %d): %.1fx%.1f", page,
		     pw, ph);
		return;
	}

	FPDF_PAGE pg = pFPDF_LoadPage(s->doc, page - 1);
	if (!pg) {
		blog(LOG_WARNING, "[pdf-presenter] LoadPage 失败 (page %d)", page);
		return;
	}

	/* 以 1920x1080 为上限等比缩放页面，画布 = 页面实际渲染尺寸 */
	float fit = std::min((float)CANVAS_W / (float)pw, (float)CANVAS_H / (float)ph);
	int dw = (int)std::lround(pw * fit);
	int dh = (int)std::lround(ph * fit);
	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;

	FPDF_BITMAP bmp = pFPDFBitmap_Create(dw, dh, 1);
	if (!bmp) {
		blog(LOG_WARNING, "[pdf-presenter] FPDFBitmap_Create 失败 (%dx%d)", dw, dh);
		pFPDF_ClosePage(pg);
		return;
	}
	pFPDFBitmap_FillRect(bmp, 0, 0, dw, dh, 0xFFFFFFFF);
	pFPDF_RenderPageBitmap(bmp, pg, 0, 0, dw, dh, 0, 0);
	const uint8_t *src = (const uint8_t *)pFPDFBitmap_GetBuffer(bmp);
	if (!src) {
		blog(LOG_WARNING, "[pdf-presenter] Bitmap_GetBuffer 失败");
		pFPDFBitmap_Destroy(bmp);
		pFPDF_ClosePage(pg);
		return;
	}

	/* 画布 = 页面渲染尺寸（BGRA），直接拷贝，不强制 1920x1080 */
	s->canvas.resize((size_t)dw * dh * 4);
	memcpy(s->canvas.data(), src, (size_t)dw * dh * 4);
	s->canvasW = dw;
	s->canvasH = dh;

	pFPDFBitmap_Destroy(bmp);
	pFPDF_ClosePage(pg);

	/* 只更新 CPU 画布，纹理在 video_render（渲染线程）里上传 */
	s->canvasDirty = true;

	/* 若控制窗打开，刷新页数显示 */
	if (s->cw && s->cw->hwnd)
		PostMessageW(s->cw->hwnd, WM_CTRL_REFRESH,
			     (WPARAM)(intptr_t)page,
			     (LPARAM)(intptr_t)s->pageCount);

	blog(LOG_INFO, "[pdf-presenter] 已渲染第 %d/%d 页 (%.1fx%.1f -> %dx%d)", page,
	     s->pageCount, pw, ph, dw, dh);
}

static void load_document(struct pdf_source *s)
{
	if (s->doc) {
		pFPDF_CloseDocument(s->doc);
		s->doc = nullptr;
	}
	s->pageCount = 0;
	s->pdfPath.clear();
	if (s->srcFile.empty())
		return;
	if (!pdfium_ok())
		return;

	std::string path = s->srcFile;
	std::string ext = get_ext(path);
	if (ext == "ppt" || ext == "pptx") {
		/* 按用户要求：插件只直接播放 PDF，PPTX 请先转为 PDF 再加载 */
		blog(LOG_INFO,
		     "[pdf-presenter] 检测到 PPT/PPTX。本插件只直接播放 PDF，请先用 PowerPoint 将该文件另存为 PDF 再加载。");
		return;
	}
	s->pdfPath = path;
	s->doc = pFPDF_LoadDocument(path.c_str(), nullptr);
	if (!s->doc) {
		blog(LOG_WARNING, "[pdf-presenter] 无法打开文档: %s", path.c_str());
		return;
	}
	s->pageCount = pFPDF_GetPageCount(s->doc);
	blog(LOG_INFO, "[pdf-presenter] 已载入 %s（%d 页）", path.c_str(),
	     s->pageCount);
}

/* 翻页并应用（改设置后触发 update 统一重渲染） */
static void advance(struct pdf_source *s, int delta)
{
	int page = (int)obs_data_get_int(s->settings, "page") + delta;
	if (page < 1)
		page = 1;
	if (s->pageCount > 0 && page > s->pageCount)
		page = s->pageCount;
	obs_data_set_int(s->settings, "page", page);
	obs_source_update(s->source, s->settings);
}

/* 自动播放：libobs 每帧回调（不用依赖前端 API 的 obs_timer_*） */
static void video_tick(void *data, float seconds)
{
	struct pdf_source *s = (struct pdf_source *)data;
	if (!s->autoplay || s->pageCount <= 0)
		return;
	s->acc += seconds;
	if (s->acc >= (float)s->interval) {
		s->acc = 0;
		advance(s, 1);
	}
}

static void hotkey_next(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		advance((struct pdf_source *)data, 1);
}

static void hotkey_prev(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		advance((struct pdf_source *)data, -1);
}

static bool prev_btn(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	advance((struct pdf_source *)data, -1);
	return false; // 保持属性窗口打开，方便连点
}

/* ============================= 悬浮控制窗 ============================= */
struct advance_job {
	struct pdf_source *s;
	int delta;
};
static void advance_job_run(void *param)
{
	struct advance_job *j = (struct advance_job *)param;
	advance(j->s, j->delta);
	delete j;
}
/* 从控制窗线程安全地把翻页调度到 OBS UI 线程 */
static void advance_on_ui(struct pdf_source *s, int delta)
{
	struct advance_job *j = new struct advance_job{s, delta};
	obs_queue_task(OBS_TASK_UI, advance_job_run, j, false);
}

static LRESULT CALLBACK ctrl_wndproc(HWND hwnd, UINT msg, WPARAM wParam,
				     LPARAM lParam)
{
	struct ctrl_window *cw =
		(struct ctrl_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case ID_CTRL_PREV:
			if (cw)
				advance_on_ui(cw->src, -1);
			return 0;
		case ID_CTRL_NEXT:
			if (cw)
				advance_on_ui(cw->src, 1);
			return 0;
		case ID_CTRL_CLOSE:
			if (cw)
				cw->active = false;
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			return 0;
		}
		break;
	case WM_CTRL_REFRESH:
		if (cw) {
			cw->page = (int)(intptr_t)wParam;
			cw->total = (int)(intptr_t)lParam;
			if (cw->label) {
				char tmp[64];
				snprintf(tmp, sizeof(tmp), "%d / %d ", cw->page,
					 cw->total);
				std::wstring ws = utf8_to_wide(tmp);
				ws += L"\x9875"; // 页
				SetWindowTextW(cw->label, ws.c_str());
			}
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ctrl_window_run(struct pdf_source *s)
{
	struct ctrl_window *cw = s->cw;
	if (!cw)
		return;
	HINSTANCE inst = GetModuleHandleW(nullptr);
	const wchar_t *cls = L"ObsPdfPresenterCtrlWnd";
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = ctrl_wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = cls;
	RegisterClassExW(&wc);

	std::wstring title = utf8_to_wide("演示文稿控制 - ");
	const char *nm = obs_source_get_name(s->source);
	title += utf8_to_wide(nm ? nm : "");

	DWORD exstyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
	HWND hwnd = CreateWindowExW(exstyle, cls, title.c_str(),
				    WS_POPUP | WS_VISIBLE, 120, 120, 214, 36,
				    nullptr, nullptr, inst, nullptr);
	if (!hwnd)
		return;
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cw);
	cw->hwnd = hwnd;

	HFONT font = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0,
				 DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei UI");
	HWND prev = CreateWindowExW(0, L"BUTTON", L"<",
				    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 2, 2,
				    44, 32, hwnd, (HMENU)ID_CTRL_PREV, inst,
				    nullptr);
	HWND label = CreateWindowExW(0, L"STATIC", L"0 / 0",
				     WS_CHILD | WS_VISIBLE | SS_CENTER |
					     SS_CENTERIMAGE,
				     48, 2, 88, 32, hwnd, nullptr, inst,
				     nullptr);
	HWND next = CreateWindowExW(0, L"BUTTON", L">",
				    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 138,
				    2, 44, 32, hwnd, (HMENU)ID_CTRL_NEXT, inst,
				    nullptr);
	HWND close = CreateWindowExW(0, L"BUTTON", L"x",
				     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 184,
				     2, 28, 32, hwnd, (HMENU)ID_CTRL_CLOSE,
				     inst, nullptr);
	SendMessageW(prev, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(next, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(close, WM_SETFONT, (WPARAM)font, TRUE);
	cw->label = label;

	PostMessageW(hwnd, WM_CTRL_REFRESH, (WPARAM)(intptr_t)s->curPage,
		     (LPARAM)(intptr_t)s->pageCount);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	cw->hwnd = nullptr;
}

static void open_ctrl_window(struct pdf_source *s)
{
	if (s->cw || s->pageCount <= 0)
		return;
	struct ctrl_window *cw = new struct ctrl_window();
	cw->active = true;
	cw->src = s;
	s->cw = cw;
	cw->thd = std::thread([s]() { ctrl_window_run(s); });
}

static void close_ctrl_window(struct pdf_source *s)
{
	struct ctrl_window *cw = s->cw;
	if (!cw)
		return;
	cw->active = false;
	if (cw->hwnd)
		PostMessageW(cw->hwnd, WM_CLOSE, 0, 0);
	if (cw->thd.joinable())
		cw->thd.join();
	s->cw = nullptr;
	delete cw;
}

static bool ctrl_btn(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct pdf_source *s = (struct pdf_source *)data;
	if (s->cw)
		close_ctrl_window(s);
	else
		open_ctrl_window(s);
	return true; // 刷新属性，更新按钮文字
}

/* ============ OBS「可控制媒体源」接口：让画布上出现原生播放控制条 ============ */
static void media_play_pause(void *data, bool pause)
{
	struct pdf_source *s = (struct pdf_source *)data;
	s->autoplay = !pause;
	obs_data_set_bool(s->settings, "autoplay", !pause);
	obs_source_update(s->source, s->settings);
}

static void media_restart(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	obs_data_set_int(s->settings, "page", 1);
	obs_source_update(s->source, s->settings);
}

static void media_stop(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	s->autoplay = false;
	obs_data_set_bool(s->settings, "autoplay", false);
	obs_source_update(s->source, s->settings);
}

static void media_next(void *data)
{
	advance((struct pdf_source *)data, 1);
}

static void media_previous(void *data)
{
	advance((struct pdf_source *)data, -1);
}

static int64_t media_get_duration(void *data)
{
	UNUSED_PARAMETER(data);
	return 0; // 与原生图像幻灯片一致：不提供时长，控制条显示“- / -”
}

static int64_t media_get_time(void *data)
{
	UNUSED_PARAMETER(data);
	return 0;
}

static enum obs_media_state media_get_state(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	return s->autoplay ? OBS_MEDIA_STATE_PLAYING : OBS_MEDIA_STATE_PAUSED;
}

static bool next_btn(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	advance((struct pdf_source *)data, 1);
	return false;
}

static void update(void *data, obs_data_t *settings)
{
	struct pdf_source *s = (struct pdf_source *)data;
	s->autoplay = obs_data_get_bool(settings, "autoplay");
	s->interval = (int)obs_data_get_int(settings, "interval");
	if (s->interval < 1)
		s->interval = 1;

	const char *f = obs_data_get_string(settings, "file");
	std::string nf = f ? normalize_path(f) : "";
	if (nf != s->srcFile) {
		s->srcFile = nf;
		load_document(s);
		s->curPage = 0;
		s->acc = 0;
	}

	if (!s->srcFile.empty() && s->doc) {
		int target = (int)obs_data_get_int(settings, "page");
		if (target < 1)
			target = 1;
		if (s->pageCount > 0 && target > s->pageCount)
			target = s->pageCount;
		if (target != s->curPage) {
			s->curPage = target;
			render_page(s, target);
		}
	}
}

static void video_render(void *data, gs_effect_t *effect_unused)
{
	UNUSED_PARAMETER(effect_unused);
	struct pdf_source *s = (struct pdf_source *)data;
	if (s->canvas.empty())
		return;

	/* 纹理创建/上传必须在渲染线程内进行，画布尺寸变化时重建纹理 */
	if (!s->tex || s->texW != s->canvasW || s->texH != s->canvasH) {
		if (s->tex) {
			gs_texture_destroy(s->tex);
			s->tex = nullptr;
		}
		s->tex = gs_texture_create(s->canvasW, s->canvasH, GS_BGRA, 1, nullptr,
					   GS_DYNAMIC);
		s->texW = s->canvasW;
		s->texH = s->canvasH;
	}
	if (s->tex && s->canvasDirty) {
		gs_texture_set_image(s->tex, s->canvas.data(), s->canvasW * 4, false);
		s->canvasDirty = false;
	}
	if (!s->tex)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), s->tex);

	/* 直接按当前矩阵绘制（含 OBS 的位置/缩放/旋转变换），不重置矩阵 */
	gs_draw_sprite(s->tex, 0, 0, 0);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static uint32_t get_width(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	return s->canvasW;
}

static uint32_t get_height(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	return s->canvasH;
}

static obs_properties_t *get_properties(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_path(props, "file", obs_module_text("File"),
				OBS_PATH_FILE, "PDF (*.pdf)", nullptr);
	obs_properties_add_int(props, "page", obs_module_text("Page"), 1, 100000, 1);
	obs_properties_add_button(props, "prev", obs_module_text("Prev"), prev_btn);
	obs_properties_add_button(props, "next", obs_module_text("Next"), next_btn);
	obs_properties_add_button(
		props, "ctrl",
		(s->cw ? obs_module_text("CloseControl")
			: obs_module_text("OpenControl")),
		ctrl_btn);
	obs_properties_add_bool(props, "autoplay", obs_module_text("Autoplay"));
	obs_properties_add_int(props, "interval", obs_module_text("Interval"), 1,
				3600, 1);
	UNUSED_PARAMETER(s);
	return props;
}

static void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "file", "");
	obs_data_set_default_int(settings, "page", 1);
	obs_data_set_default_bool(settings, "autoplay", false);
	obs_data_set_default_int(settings, "interval", 5);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct pdf_source *s = new struct pdf_source();
	s->source = source;
	s->settings = settings;
	obs_data_addref(s->settings);

	obs_source_update(s->source, s->settings);

	obs_hotkey_register_source(source, "obs-pdf-presenter.next",
				   obs_module_text("Hotkey.Next"), hotkey_next, s);
	obs_hotkey_register_source(source, "obs-pdf-presenter.prev",
				   obs_module_text("Hotkey.Prev"), hotkey_prev, s);
	return s;
}

static void destroy(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	if (!s)
		return;
	if (s->cw)
		close_ctrl_window(s);
	if (s->doc)
		pFPDF_CloseDocument(s->doc);
	if (s->tex)
		gs_texture_destroy(s->tex);
	obs_data_release(s->settings);
	delete s;
}

/* ============================= 模块注册 ============================= */
static struct obs_source_info source_info = {};
static bool registered = false;

bool obs_module_load(void)
{
	/* 定位插件目录，供 pdfium.dll 使用 */
	obs_module_t *mod = obs_current_module();
	const char *bin = mod ? obs_get_module_binary_path(mod) : nullptr;
	if (bin) {
		std::string b = bin;
		size_t p = b.find_last_of("/\\");
		g_module_dir = (p == std::string::npos) ? b : b.substr(0, p);
		g_module_dir = normalize_path(g_module_dir);
	}

	load_pdfium();

	source_info.id = "obs_pdf_presenter_source";
	source_info.type = OBS_SOURCE_TYPE_INPUT;
	source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CONTROLLABLE_MEDIA;
	source_info.get_name = get_name;
	source_info.create = create;
	source_info.destroy = destroy;
	source_info.get_width = get_width;
	source_info.get_height = get_height;
	source_info.get_defaults = get_defaults;
	source_info.get_properties = get_properties;
	source_info.update = update;
	source_info.video_render = video_render;
	source_info.video_tick = video_tick;

	source_info.media_play_pause = media_play_pause;
	source_info.media_restart = media_restart;
	source_info.media_stop = media_stop;
	source_info.media_next = media_next;
	source_info.media_previous = media_previous;
	source_info.media_get_duration = media_get_duration;
	source_info.media_get_time = media_get_time;
	source_info.media_get_state = media_get_state;

	obs_register_source(&source_info);
	registered = true;
	blog(LOG_INFO, "[pdf-presenter] 演示文稿播放器插件已加载 (v%s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	if (registered && pdfium_ok())
		pFPDF_DestroyLibrary();
	registered = false;
	blog(LOG_INFO, "[pdf-presenter] 插件已卸载");
}
