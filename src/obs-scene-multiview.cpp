/*
 * obs-scene-multiview —— OBS 原生「多视图场景切换」插件（Win32 版，不依赖 Qt）
 * 独立的浮动窗口：把当前场景集合里的所有场景按数量动态排成网格，
 * 每个格子显示场景实时缩略图与名称；点一下选中，按「切换」即把该
 * 场景设为主场景（也可双击直接切换）。场景数不限，格子自动排布。
 *
 * 缩略图渲染：在 OBS graphics 线程把每个场景渲染到纹理，再读回 CPU
 * 缓冲，UI 线程用 GDI 画进各格子。
 *
 * 本项目按 GNU GPL v2 协议开源。
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <vector>
#include <mutex>
#include <string>

/* UTF-8 -> 宽字符（本文件自用，避免依赖主插件的私有函数） */
static std::wstring utf8_to_wide(const char *s)
{
	if (!s || !*s)
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring r((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s, -1, &r[0], n);
	return r;
}

/* ---------------------- 场景缩略图渲染 ---------------------- */

struct scene_tile {
	obs_source_t *scene = nullptr;
	gs_texture_t *tex = nullptr;
	gs_stagesurf_t *stage = nullptr;
	unsigned char *buf = nullptr; // texW*texH*4 RGBA 缓冲
	int texW = 0;
	int texH = 0;
	bool ready = false; // buf 已有本次渲染结果
	HWND hwnd = nullptr;
};

#define THUMB_MAX_H 180 /* 缩略图最大高度像素 */

#define IDC_SWITCH 1001
#define IDC_REFRESH 1002
#define TIMER_RENDER 1

/* 网格布局参数 */
#define TILE_W 220
#define TILE_H 150
#define GAP 8
#define TOPBAR_H 34
#define MARGIN 10

struct multiview_ctx {
	HWND win = nullptr;
	HWND statusLabel = nullptr;
	HWND switchBtn = nullptr;
	HWND refreshBtn = nullptr;
	HWND gridHost = nullptr;

	std::mutex mtx;
	std::vector<scene_tile> tiles;
	obs_source_t *selected = nullptr;
	int selectedIndex = -1;
	int cols = 4;
	int scrollPos = 0;
	int rows = 0;
	bool winOpen = false;

	HFONT font = nullptr;
	HFONT fontBig = nullptr;
};

static multiview_ctx *g_ctx = nullptr;

/* 在一个场景的纹理上渲染它（必须在 graphics 线程调用） */
static void render_one_tile(scene_tile &t)
{
	if (!t.tex || !t.scene)
		return;
	gs_set_render_target(t.tex, nullptr);
	gs_viewport_push();
	gs_set_viewport(0, 0, t.texW, t.texH);
	gs_ortho(0.0f, (float)t.texW, 0.0f, (float)t.texH, -100.0f, 100.0f);
	obs_source_video_render(t.scene);
	gs_viewport_pop();
	gs_set_render_target(nullptr, nullptr);
	gs_flush();

	if (t.stage) {
		gs_stage_texture(t.stage, t.tex);
		unsigned char *ptr = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(t.stage, &ptr, &linesize)) {
			unsigned char *dst = t.buf;
			for (int y = 0; y < t.texH; y++) {
				memcpy(dst, ptr + (size_t)y * linesize,
				       (size_t)t.texW * 4);
				dst += (size_t)t.texW * 4;
			}
			gs_stagesurface_unmap(t.stage);
			t.ready = true;
		}
	}
}

/* graphics 线程任务：渲染所有场景缩略图 */
static void render_all_task(void *param)
{
	multiview_ctx *ctx = (multiview_ctx *)param;
	std::lock_guard<std::mutex> lk(ctx->mtx);
	for (auto &t : ctx->tiles)
		render_one_tile(t);
}

/* 创建/更新纹理与 stage（必须在 graphics 线程） */
static void setup_textures_task(void *param)
{
	multiview_ctx *ctx = (multiview_ctx *)param;
	std::lock_guard<std::mutex> lk(ctx->mtx);
	for (auto &t : ctx->tiles) {
		if (t.tex)
			continue;
		t.tex = gs_texture_create((uint32_t)t.texW, (uint32_t)t.texH,
					 GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
		t.stage = gs_stagesurface_create((uint32_t)t.texW, (uint32_t)t.texH,
						 GS_RGBA);
	}
}

/* 释放全部 tiles 的 GPU 资源与缓冲（必须在 graphics 线程） */
static void release_tiles_task(void *param)
{
	multiview_ctx *ctx = (multiview_ctx *)param;
	std::lock_guard<std::mutex> lk(ctx->mtx);
	for (auto &t : ctx->tiles) {
		if (t.tex)
			gs_texture_destroy(t.tex);
		if (t.stage)
			gs_stagesurface_destroy(t.stage);
		if (t.buf)
			bfree(t.buf);
		if (t.scene)
			obs_source_release(t.scene);
	}
	ctx->tiles.clear();
}

/* ---------------------- GDI 绘制 ---------------------- */

static void tile_paint(multiview_ctx *ctx, HWND hwnd, int index)
{
	scene_tile &t = ctx->tiles[index];
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);
	RECT rc;
	GetClientRect(hwnd, &rc);

	/* 背景 */
	HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
	FillRect(hdc, &rc, bg);
	DeleteObject(bg);

	/* 缩略图 */
	{
		std::lock_guard<std::mutex> lk(ctx->mtx);
		if (t.ready && t.buf) {
			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = t.texW;
			bmi.bmiHeader.biHeight = -t.texH; /* top-down */
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_BITFIELDS;
			((DWORD *)&bmi.bmiColors)[0] = 0x00FF0000; /* R */
			((DWORD *)&bmi.bmiColors)[1] = 0x0000FF00; /* G */
			((DWORD *)&bmi.bmiColors)[2] = 0x000000FF; /* B */
			int iw = rc.right - rc.left;
			int ih = rc.bottom - rc.top;
			SetStretchBltMode(hdc, HALFTONE);
			StretchDIBits(hdc, 0, 0, iw, ih - 20, 0, 0, t.texW, t.texH,
				      t.buf, &bmi, DIB_RGB_COLORS, SRCCOPY);
		}
	}
	/* 场景名 */
	if (t.scene) {
		SetTextColor(hdc, RGB(230, 230, 230));
		SetBkMode(hdc, TRANSPARENT);
		RECT tr = {0, rc.bottom - 18, rc.right, rc.bottom};
		HGDIOBJ of = SelectObject(hdc, ctx->font);
		DrawTextW(hdc, utf8_to_wide(obs_source_get_name(t.scene)).c_str(),
			  -1, &tr,
			  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		SelectObject(hdc, of);
	}

	/* 选中描边 */
	if (index == ctx->selectedIndex) {
		HBRUSH sel = CreateSolidBrush(RGB(38, 160, 218));
		FrameRect(hdc, &rc, sel);
		DeleteObject(sel);
	}
	EndPaint(hwnd, &ps);
}

/* ---------------------- 窗口过程 ---------------------- */

static void select_tile(multiview_ctx *ctx, HWND hwnd)
{
	int index = (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA) - 1;
	if (index < 0 || index >= (int)ctx->tiles.size())
		return;
	ctx->selected = ctx->tiles[index].scene;
	ctx->selectedIndex = index;
	EnableWindow(ctx->switchBtn, TRUE);
	/* 重画所有格子刷新选中框 */
	for (size_t i = 0; i < ctx->tiles.size(); i++)
		if (ctx->tiles[i].hwnd)
			InvalidateRect(ctx->tiles[i].hwnd, nullptr, TRUE);
}

static void switch_to_tile(multiview_ctx *ctx, HWND hwnd)
{
	int index = (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA) - 1;
	if (index < 0 || index >= (int)ctx->tiles.size())
		return;
	ctx->selected = ctx->tiles[index].scene;
	ctx->selectedIndex = index;
	obs_frontend_set_current_scene(ctx->selected);
	SetWindowTextW(ctx->statusLabel,
		       (L"已切换到：" +
			utf8_to_wide(obs_source_get_name(ctx->selected)))
			       .c_str());
}

static LRESULT CALLBACK tile_wndproc(HWND hwnd, UINT msg, WPARAM wParam,
				     LPARAM lParam)
{
	multiview_ctx *ctx = g_ctx;
	switch (msg) {
	case WM_PAINT: {
		int index = (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA) - 1;
		if (ctx && index >= 0 && index < (int)ctx->tiles.size())
			tile_paint(ctx, hwnd, index);
		return 0;
	}
	case WM_LBUTTONUP:
		if (ctx)
			select_tile(ctx, hwnd);
		return 0;
	case WM_LBUTTONDBLCLK:
		if (ctx)
			switch_to_tile(ctx, hwnd);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* 布局：根据窗口大小摆放顶部栏与网格区域，并排列格子 */
static void layout_tiles(multiview_ctx *ctx)
{
	if (!ctx->win)
		return;
	RECT cr;
	GetClientRect(ctx->win, &cr);
	int w = cr.right - cr.left;
	int h = cr.bottom - cr.top;
	int top = 4;
	SetWindowPos(ctx->statusLabel, nullptr, 6, top, w - 260, 24, SWP_NOZORDER);
	SetWindowPos(ctx->refreshBtn, nullptr, w - 190, top - 3, 80, 26,
		     SWP_NOZORDER);
	SetWindowPos(ctx->switchBtn, nullptr, w - 100, top - 3, 90, 26,
		     SWP_NOZORDER);
	SetWindowPos(ctx->gridHost, nullptr, 0, TOPBAR_H, w, h - TOPBAR_H,
		     SWP_NOZORDER);

	size_t n = ctx->tiles.size();
	int cols = (n >= 8) ? 5 : ((n >= 4) ? 4 : 3);
	if (cols < 1)
		cols = 1;
	ctx->cols = cols;
	int rows = (int)((n + cols - 1) / cols);
	ctx->rows = rows;

	/* 可滚动内容高度 */
	int hostH = (h - TOPBAR_H);
	int contentH = rows * (TILE_H + GAP) + MARGIN * 2;
	int maxScroll = contentH - hostH;
	if (maxScroll < 0)
		maxScroll = 0;
	SCROLLINFO si = {};
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE;
	si.nMin = 0;
	si.nMax = contentH;
	si.nPage = hostH;
	SetScrollInfo(ctx->gridHost, SB_VERT, &si, TRUE);
	if (ctx->scrollPos > maxScroll)
		ctx->scrollPos = maxScroll;

	for (size_t i = 0; i < n; i++) {
		int r = (int)(i / cols);
		int c = (int)(i % cols);
		int x = MARGIN + c * (TILE_W + GAP);
		int y = MARGIN + r * (TILE_H + GAP) - ctx->scrollPos;
		if (ctx->tiles[i].hwnd)
			SetWindowPos(ctx->tiles[i].hwnd, nullptr, x, y, TILE_W,
				     TILE_H, SWP_NOZORDER);
	}
}

/* 重建 tiles 数组 + 格子窗口 + 布局 */
static void rebuild_grid(multiview_ctx *ctx)
{
	if (!ctx->gridHost)
		return;
	/* 释放上一批 GPU 资源（graphics 线程） */
	obs_queue_task(OBS_TASK_GRAPHICS, release_tiles_task, ctx, true);

	/* 销毁旧格子窗口 */
	for (auto &t : ctx->tiles)
		if (t.hwnd)
			DestroyWindow(t.hwnd);
	ctx->tiles.clear();
	ctx->selected = nullptr;
	ctx->selectedIndex = -1;
	EnableWindow(ctx->switchBtn, FALSE);

	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	size_t n = list.sources.num;
	ctx->tiles.resize(n);

	/* 创建格子窗口 */
	HINSTANCE inst = (HINSTANCE)GetModuleHandleW(nullptr);
	for (size_t i = 0; i < n; i++) {
		scene_tile &t = ctx->tiles[i];
		t.scene = obs_source_get_ref(list.sources.array[i]);
		uint32_t sw = obs_source_get_width(list.sources.array[i]);
		uint32_t sh = obs_source_get_height(list.sources.array[i]);
		if (sw == 0 || sh == 0) {
			sw = 320;
			sh = 180;
		}
		int h = (int)sh, w = (int)sw;
		if (h > THUMB_MAX_H) {
			w = (int)((float)w * THUMB_MAX_H / h);
			h = THUMB_MAX_H;
		}
		t.texW = w;
		t.texH = h;
		t.buf = (unsigned char *)bmalloc((size_t)w * h * 4);
		t.hwnd = CreateWindowExW(0, L"MV_Tile", L"",
					 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
					 0, 0, TILE_W, TILE_H, ctx->gridHost,
					 nullptr, inst, nullptr);
		if (t.hwnd) {
			SetWindowLongPtrW(t.hwnd, GWLP_USERDATA, (LONG_PTR)(i + 1));
			SetWindowFont(t.hwnd, ctx->font, TRUE);
		}
	}

	if (n == 0) {
		SetWindowTextW(ctx->statusLabel, L"当前没有场景");
	} else {
		wchar_t buf[64];
		swprintf_s(buf, L"共 %u 个场景（点一下选中，点「切换」切换，双击直接切换）",
			   (unsigned)n);
		SetWindowTextW(ctx->statusLabel, buf);
	}

	obs_frontend_source_list_free(&list);

	/* 创建纹理并渲染一次 */
	obs_queue_task(OBS_TASK_GRAPHICS, setup_textures_task, ctx, true);
	obs_queue_task(OBS_TASK_GRAPHICS, render_all_task, ctx, true);
	layout_tiles(ctx);
	for (auto &t : ctx->tiles)
		if (t.hwnd)
			InvalidateRect(t.hwnd, nullptr, TRUE);
}

static void on_refresh(multiview_ctx *ctx)
{
	rebuild_grid(ctx);
}

static void on_switch(multiview_ctx *ctx)
{
	if (!ctx->selected)
		return;
	obs_frontend_set_current_scene(ctx->selected);
	SetWindowTextW(ctx->statusLabel,
		       (L"已切换到：" +
			utf8_to_wide(obs_source_get_name(ctx->selected)))
			       .c_str());
}

static void close_window(multiview_ctx *ctx);

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wParam,
				     LPARAM lParam)
{
	multiview_ctx *ctx = g_ctx;
	switch (msg) {
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_SWITCH && ctx)
			on_switch(ctx);
		else if (LOWORD(wParam) == IDC_REFRESH && ctx)
			on_refresh(ctx);
		return 0;
	case WM_SIZE:
		if (ctx)
			layout_tiles(ctx);
		return 0;
	case WM_VSCROLL: {
		if (!ctx)
			break;
		SCROLLINFO si = {};
		si.cbSize = sizeof(si);
		si.fMask = SIF_ALL;
		GetScrollInfo(ctx->gridHost, SB_VERT, &si);
		int newPos = si.nPos;
		switch (LOWORD(wParam)) {
		case SB_LINEUP:
			newPos -= 12;
			break;
		case SB_LINEDOWN:
			newPos += 12;
			break;
		case SB_PAGEUP:
			newPos -= si.nPage;
			break;
		case SB_PAGEDOWN:
			newPos += si.nPage;
			break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION:
			newPos = HIWORD(wParam);
			break;
		}
		if (newPos < si.nMin)
			newPos = si.nMin;
		if (newPos > si.nMax - (int)si.nPage)
			newPos = si.nMax - (int)si.nPage;
		ctx->scrollPos = newPos;
		SetScrollPos(ctx->gridHost, SB_VERT, newPos, TRUE);
		layout_tiles(ctx);
		InvalidateRect(ctx->gridHost, nullptr, TRUE);
		return 0;
	}
	case WM_TIMER:
		if (ctx && wParam == TIMER_RENDER) {
			obs_queue_task(OBS_TASK_GRAPHICS, render_all_task, ctx,
				       false);
			for (auto &t : ctx->tiles)
				if (t.hwnd)
					InvalidateRect(t.hwnd, nullptr, TRUE);
		}
		return 0;
	case WM_CLOSE:
		if (ctx)
			close_window(ctx); /* 先释放资源 */
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		if (ctx)
			ctx->win = nullptr; /* 窗口已销毁 */
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* 打开/创建窗口 */
static void open_window(multiview_ctx *ctx)
{
	if (ctx->winOpen)
		return;
	ctx->winOpen = true;

	HINSTANCE inst = (HINSTANCE)GetModuleHandleW(nullptr);
	ctx->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
	ctx->fontBig =
		CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
			    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

	ctx->win = CreateWindowExW(0, L"MV_Window", L"多视图场景切换",
				   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
				   CW_USEDEFAULT, CW_USEDEFAULT, 1160, 700,
				   nullptr, nullptr, inst, nullptr);
	if (!ctx->win) {
		ctx->winOpen = false;
		return;
	}

	ctx->statusLabel =
		CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 6, 6,
				600, 24, ctx->win, nullptr, inst, nullptr);
	ctx->refreshBtn = CreateWindowExW(0, L"BUTTON", L"刷新",
					  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
					  0, 0, 80, 26, ctx->win,
					  (HMENU)IDC_REFRESH, inst, nullptr);
	ctx->switchBtn = CreateWindowExW(0, L"BUTTON", L"切换",
					 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
					 0, 0, 90, 26, ctx->win,
					 (HMENU)IDC_SWITCH, inst, nullptr);
	ctx->gridHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"MV_Grid", L"",
					WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0,
					TOPBAR_H, 1160, 660, ctx->win, nullptr,
					inst, nullptr);

	SetWindowFont(ctx->statusLabel, ctx->font, TRUE);
	SetWindowFont(ctx->refreshBtn, ctx->font, TRUE);
	SetWindowFont(ctx->switchBtn, ctx->font, TRUE);

	rebuild_grid(ctx);
	SetTimer(ctx->win, TIMER_RENDER, 300, nullptr);

	ShowWindow(ctx->win, SW_SHOW);
}

static void close_window(multiview_ctx *ctx)
{
	if (!ctx->winOpen)
		return;
	ctx->winOpen = false;
	if (ctx->win)
		KillTimer(ctx->win, TIMER_RENDER);
	obs_queue_task(OBS_TASK_GRAPHICS, release_tiles_task, ctx, true);
	for (auto &t : ctx->tiles)
		if (t.hwnd)
			DestroyWindow(t.hwnd);
	ctx->tiles.clear();
	if (ctx->font)
		DeleteObject(ctx->font);
	if (ctx->fontBig)
		DeleteObject(ctx->fontBig);
	ctx->font = nullptr;
	ctx->fontBig = nullptr;
	ctx->selected = nullptr;
	ctx->selectedIndex = -1;
	ctx->statusLabel = nullptr;
	ctx->switchBtn = nullptr;
	ctx->refreshBtn = nullptr;
	ctx->gridHost = nullptr;
}

/* ---------------------- 插件入口（由主插件调用） ---------------------- */

static void open_multiview(void *data)
{
	multiview_ctx *ctx = (multiview_ctx *)data;
	open_window(ctx);
}

bool multiview_module_load(void)
{
	HINSTANCE inst = (HINSTANCE)GetModuleHandleW(nullptr);
	WNDCLASSEXW tmp = {};
	tmp.cbSize = sizeof(tmp);
	if (!GetClassInfoExW(inst, L"MV_Window", &tmp)) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = main_wndproc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
		wc.lpszClassName = L"MV_Window";
		RegisterClassW(&wc);
	}
	if (!GetClassInfoExW(inst, L"MV_Tile", &tmp)) {
		WNDCLASSW wt = {};
		wt.lpfnWndProc = tile_wndproc;
		wt.hInstance = inst;
		wt.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_HAND);
		wt.lpszClassName = L"MV_Tile";
		RegisterClassW(&wt);
	}
	if (!GetClassInfoExW(inst, L"MV_Grid", &tmp)) {
		WNDCLASSW wg = {};
		wg.lpfnWndProc = DefWindowProcW;
		wg.hInstance = inst;
		wg.hbrBackground = (HBRUSH)GetStockObject(COLOR_BTNFACE);
		wg.lpszClassName = L"MV_Grid";
		RegisterClassW(&wg);
	}

	g_ctx = new multiview_ctx();
	obs_frontend_add_tools_menu_item("多视图场景切换（动态网格）", open_multiview,
					 g_ctx);
	return true;
}

void multiview_module_unload(void)
{
	if (g_ctx) {
		close_window(g_ctx);
		if (g_ctx->win)
			DestroyWindow(g_ctx->win);
		delete g_ctx;
		g_ctx = nullptr;
	}
	UnregisterClassW(L"MV_Window", (HINSTANCE)GetModuleHandleW(nullptr));
	UnregisterClassW(L"MV_Tile", (HINSTANCE)GetModuleHandleW(nullptr));
	UnregisterClassW(L"MV_Grid", (HINSTANCE)GetModuleHandleW(nullptr));
}
