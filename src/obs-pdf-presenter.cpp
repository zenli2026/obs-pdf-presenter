/*
 * obs-pdf-presenter —— OBS 原生「演示文稿播放器」插件
 * 在 OBS 来源列表直接出现「演示文稿播放器」，可选 PDF / PPT / PPTX，
 * 支持快捷键翻页、跳页、自动播放。PDF 用 PDFium 渲染，PPT 用本机
 * PowerPoint(COM) 转 PDF 后渲染。
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
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>

#define PLUGIN_NAME    "obs-pdf-presenter"
#define PLUGIN_VERSION "1.0.0"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "zh-CN")

/* ============================= 运行时定位的资源 ============================= */
static std::string g_module_dir;   // 插件 dll 所在目录（放 pdfium.dll、ppt2pdf.ps1）

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

/* 用本机 PowerPoint(COM) 把 PPT/PPTX 转成 PDF，返回生成的 pdf 路径；失败返回空 */
static bool file_exists(const std::wstring &path)
{
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::string convert_ppt_to_pdf(const std::string &src)
{
	std::wstring srcW = utf8_to_wide(src);

	wchar_t tempDir[MAX_PATH] = {0};
	if (!GetTempPathW(MAX_PATH, tempDir))
		return "";
	std::wstring outDir = std::wstring(tempDir) + L"obs_pdf_presenter\\";
	CreateDirectoryW(outDir.c_str(), nullptr);

	/* 输出名：源文件名.pdf */
	std::string base = src;
	size_t slash = base.find_last_of("/\\");
	if (slash != std::string::npos)
		base = base.substr(slash + 1);
	size_t dot = base.find_last_of('.');
	if (dot != std::string::npos)
		base = base.substr(0, dot);
	std::wstring outW = outDir + utf8_to_wide(base) + L".pdf";
	if (file_exists(outW))
		DeleteFileW(outW.c_str());

	std::wstring ps1 = utf8_to_wide(g_module_dir) + L"\\ppt2pdf.ps1";
	if (!file_exists(ps1)) {
		blog(LOG_WARNING, "[pdf-presenter] 未找到 ppt2pdf.ps1，无法转换 PPT");
		return "";
	}

	std::wstring cmd = L"\"powershell\" -NoProfile -ExecutionPolicy Bypass -File \""
			   + ps1 + L"\" -InputPath \"" + srcW + L"\" -OutputPath \""
			   + outW + L"\"";
	blog(LOG_INFO, "[pdf-presenter] 转换 PPT -> PDF: %ls", cmd.c_str());

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
			    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		blog(LOG_WARNING, "[pdf-presenter] 启动 powershell 失败: %lu",
		     GetLastError());
		return "";
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (code != 0 || !file_exists(outW)) {
		blog(LOG_WARNING, "[pdf-presenter] PPT 转换失败，退出码 %lu", code);
		return "";
	}
	return wide_to_utf8(outW);
}

/* ============================= 源结构 ============================= */
#define CANVAS_W 1920
#define CANVAS_H 1080

struct pdf_source {
	obs_source_t *source;
	obs_data_t *settings;

	std::string srcFile;   // 用户选择的原始文件
	std::string pdfPath;   // 实际被渲染的 pdf（PPT 转换后）
	FPDF_DOCUMENT doc = nullptr;
	int pageCount = 0;
	int curPage = 0;

	gs_texture_t *tex = nullptr;
	std::vector<uint8_t> canvas; // 1920x1080 BGRA

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

	/* 按 16:9 画布等比缩放，白边填充 */
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

	/* 画布整体白色 */
	if (s->canvas.size() != (size_t)CANVAS_W * CANVAS_H * 4)
		s->canvas.assign((size_t)CANVAS_W * CANVAS_H * 4, 0xFF);
	else
		std::fill(s->canvas.begin(), s->canvas.end(), 0xFF);

	int ox = (CANVAS_W - dw) / 2;
	int oy = (CANVAS_H - dh) / 2;
	for (int y = 0; y < dh; y++) {
		memcpy(s->canvas.data() + ((size_t)(oy + y) * CANVAS_W + ox) * 4,
		       src + (size_t)y * dw * 4, (size_t)dw * 4);
	}

	pFPDFBitmap_Destroy(bmp);
	pFPDF_ClosePage(pg);

	/* 上传纹理（尺寸固定 1920x1080） */
	if (!s->tex) {
		s->tex = gs_texture_create(CANVAS_W, CANVAS_H, GS_BGRA, 1, nullptr,
					   GS_DYNAMIC);
	}
	if (s->tex)
		gs_texture_set_image(s->tex, s->canvas.data(), CANVAS_W * 4, false);

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
		path = convert_ppt_to_pdf(s->srcFile);
		if (path.empty())
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
	if (!s->tex)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), s->tex);

	uint32_t ow = obs_source_get_width(s->source);
	uint32_t oh = obs_source_get_height(s->source);
	float aspect = (float)CANVAS_W / (float)CANVAS_H;
	float tw, th;
	if ((float)ow / (float)oh > aspect) {
		tw = (float)ow;
		th = tw / aspect;
	} else {
		th = (float)oh;
		tw = th * aspect;
	}
	float sx = tw / (float)CANVAS_W;
	float sy = th / (float)CANVAS_H;

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f(((float)ow - tw) / 2.0f, ((float)oh - th) / 2.0f, 0);
	gs_matrix_scale3f(sx, sy, 1.0f);
	gs_draw_sprite(s->tex, 0, 0, 0);
	gs_matrix_pop();

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static uint32_t get_width(void *data)
{
	UNUSED_PARAMETER(data);
	return CANVAS_W;
}

static uint32_t get_height(void *data)
{
	UNUSED_PARAMETER(data);
	return CANVAS_H;
}

static obs_properties_t *get_properties(void *data)
{
	struct pdf_source *s = (struct pdf_source *)data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_path(props, "file", obs_module_text("File"),
				OBS_PATH_FILE, "PDF/PPT (*.pdf *.ppt *.pptx)", nullptr);
	obs_properties_add_int(props, "page", obs_module_text("Page"), 1, 100000, 1);
	obs_properties_add_button(props, "prev", obs_module_text("Prev"), prev_btn);
	obs_properties_add_button(props, "next", obs_module_text("Next"), next_btn);
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
	/* 定位插件目录，供 pdfium.dll / ppt2pdf.ps1 使用 */
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
	source_info.output_flags = OBS_SOURCE_VIDEO;
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
