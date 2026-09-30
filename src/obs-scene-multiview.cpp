/*
 * obs-scene-multiview —— OBS 原生「多视图场景切换」插件
 * 独立的浮动窗口：把当前场景集合里的所有场景按数量动态排成网格，
 * 每个格子显示场景实时缩略图与名称；点一下选中，按「切换」即把该
 * 场景设为主场景（也可双击直接切换）。场景数不限，格子自动排布。
 *
 * 本项目按 GNU GPL v2 协议开源。
 */
#include <obs-module.h>
#include <obs-frontend-api/obs-frontend-api.h>
#include <QApplication>
#include <QDialog>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QMouseEvent>
#include <QImage>
#include <QPixmap>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <mutex>
#include <vector>
#include <string>

/* ---------------------- 场景缩略图渲染 ---------------------- */

struct scene_tile {
	obs_source_t *scene = nullptr;
	gs_texture_t *tex = nullptr;
	gs_stagesurface_t *stage = nullptr;
	unsigned char *buf = nullptr; // texW*texH*4 CPU 缓冲
	int texW = 0;
	int texH = 0;
	bool ready = false; // 本次已渲染到 buf
};

#define THUMB_MAX_H 240 /* 缩略图最大高度像素 */

struct multiview_ctx {
	QDialog *win = nullptr;
	QScrollArea *scroll = nullptr;
	QWidget *gridContainer = nullptr;
	QGridLayout *grid = nullptr;
	QPushButton *switchBtn = nullptr;
	QPushButton *refreshBtn = nullptr;
	QLabel *statusLabel = nullptr;
	QTimer *timer = nullptr;

	std::mutex mtx;
	std::vector<scene_tile> tiles; // 与 scenes 一一对应（graphics 线程写 buf）
	obs_source_t *selected = nullptr;
	int cols = 4; // 当前网格列数
	bool winOpen = false;
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
		gs_copy_texture_to_stagesurface(t.stage, t.tex);
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

/* 更新某格子的 QLabel 缩略图（UI 线程） */
static void apply_tile_to_label(multiview_ctx *ctx, int index)
{
	scene_tile &t = ctx->tiles[index];
	if (!t.ready || !t.buf)
		return;
	QImage img(t.buf, t.texW, t.texH, QImage::Format_RGBA8888);
	QLabel *lab = ctx->grid->itemAtPosition(index / ctx->cols, index % ctx->cols)
			      ->widget()
			      ->findChild<QLabel *>("thumb");
	if (lab)
		lab->setPixmap(QPixmap::fromImage(img));
	t.ready = false;
}

static void on_timer_tick(void)
{
	multiview_ctx *ctx = g_ctx;
	if (!ctx || !ctx->winOpen)
		return;

	/* 触发 graphics 线程渲染所有场景 */
	obs_queue_task(OBS_TASK_GRAPHICS, render_all_task, ctx, false);

	/* 把已就绪的缩略图刷到界面上 */
	std::lock_guard<std::mutex> lk(ctx->mtx);
	for (size_t i = 0; i < ctx->tiles.size(); i++) {
		if (ctx->tiles[i].ready)
			apply_tile_to_label(ctx, (int)i);
	}
}

/* ---------------------- 网格布局 ---------------------- */

static void refresh_scene_list(multiview_ctx *ctx);
static void release_tiles_task(void *param);

/* 可点击的缩略图格子 */
class TileWidget : public QWidget {
	Q_OBJECT
public:
	TileWidget(QWidget *parent = nullptr) : QWidget(parent) {}
	multiview_ctx *ctx = nullptr;
	obs_source_t *scene = nullptr;
	QLabel *thumb = nullptr;
	QLabel *name = nullptr;
signals:
	void tileClicked(obs_source_t *scene);
protected:
	void mousePressEvent(QMouseEvent *ev) override
	{
		Q_EMIT tileClicked(scene);
		QWidget::mousePressEvent(ev);
	}
	void mouseDoubleClickEvent(QMouseEvent *ev) override
	{
		if (scene)
			obs_frontend_set_current_scene(scene);
		QWidget::mouseDoubleClickEvent(ev);
	}
};

/* 重新构建 UI 网格：按场景数量动态分列 */
static void rebuild_grid(multiview_ctx *ctx)
{
	if (!ctx->gridContainer)
		return;
	/* 先释放上一批 GPU 资源（graphics 线程，阻塞等待） */
	obs_queue_task(OBS_TASK_GRAPHICS, release_tiles_task, ctx, true);

	/* 清掉旧格子 */
	QLayoutItem *item;
	while ((item = ctx->grid->takeAt(0)) != nullptr) {
		if (item->widget())
			item->widget()->deleteLater();
		delete item;
	}

	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	size_t n = list.sources.num;
	int total = (int)n;

	/* 计算列数：>=8 场景时一行 5 个，4~7 一行 4 个，<=3 一行 3 个 */
	ctx->cols = (total >= 8) ? 5 : ((total >= 4) ? 4 : 3);
	if (ctx->cols < 1)
		ctx->cols = 1;

	/* 重建 tiles 数组（graphics 线程会用它） */
	{
		std::lock_guard<std::mutex> lk(ctx->mtx);
		ctx->tiles.clear();
		ctx->tiles.resize(total);
		for (size_t i = 0; i < n; i++) {
			ctx->tiles[i].scene = obs_source_get_ref(list.sources.array[i]);
			uint32_t sw = obs_source_get_width(list.sources.array[i]);
			uint32_t sh = obs_source_get_height(list.sources.array[i]);
			if (sw == 0 || sh == 0) {
				sw = 320;
				sh = 180;
			}
			/* 等比缩到高度 <= THUMB_MAX_H */
			int h = (int)sh, w = (int)sw;
			if (h > THUMB_MAX_H) {
				w = (int)((float)w * THUMB_MAX_H / h);
				h = THUMB_MAX_H;
			}
			ctx->tiles[i].texW = w;
			ctx->tiles[i].texH = h;
			ctx->tiles[i].buf =
				(unsigned char *)bmalloc((size_t)w * h * 4);
		}
	}

	if (total == 0) {
		ctx->statusLabel->setText("当前没有场景");
		return;
	}
	ctx->statusLabel->setText(QString("共 %1 个场景").arg(total));

	for (int i = 0; i < total; i++) {
		TileWidget *tw = new TileWidget(ctx->gridContainer);
		tw->ctx = ctx;
		tw->scene = list.sources.array[i];

		QVBoxLayout *vl = new QVBoxLayout(tw);
		vl->setContentsMargins(2, 2, 2, 2);
		QLabel *thumb = new QLabel(tw);
		thumb->setObjectName("thumb");
		thumb->setAlignment(Qt::AlignCenter);
		thumb->setMinimumSize(160, 90);
		thumb->setStyleSheet(
			"background:#222; border:1px solid #444;");
		thumb->setText("…");
		QLabel *nm = new QLabel(
			QString::fromUtf8(obs_source_get_name(list.sources.array[i])),
			tw);
		nm->setAlignment(Qt::AlignCenter);
		nm->setStyleSheet("color:#eee; font-size:12px;");
		vl->addWidget(thumb);
		vl->addWidget(nm);

		tw->thumb = thumb;
		tw->name = nm;

		int r = i / ctx->cols;
		int c = i % ctx->cols;
		ctx->grid->addWidget(tw, r, c);
		QObject::connect(tw, &TileWidget::tileClicked,
				 [ctx](obs_source_t *scene) {
					 ctx->selected = scene;
					 ctx->switchBtn->setEnabled(scene != nullptr);
				 });
	}

	obs_frontend_source_list_free(&list);
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
		t.stage = gs_stagesurface_create((uint32_t)t.texW,
						 (uint32_t)t.texH, GS_RGBA, 1);
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

static void refresh_scene_list(multiview_ctx *ctx)
{
	rebuild_grid(ctx);
	obs_queue_task(OBS_TASK_GRAPHICS, setup_textures_task, ctx, true);
	obs_queue_task(OBS_TASK_GRAPHICS, render_all_task, ctx, true);
	on_timer_tick();
}

static void on_switch_clicked(void)
{
	multiview_ctx *ctx = g_ctx;
	if (!ctx || !ctx->selected)
		return;
	obs_frontend_set_current_scene(ctx->selected);
	ctx->statusLabel->setText(
		QString("已切换到：%1")
			.arg(QString::fromUtf8(
				obs_source_get_name(ctx->selected))));
}

static void open_window(multiview_ctx *ctx)
{
	if (ctx->winOpen)
		return;
	ctx->winOpen = true;

	ctx->win = new QDialog();
	ctx->win->setWindowTitle("多视图场景切换");
	ctx->win->resize(1000, 640);

	QVBoxLayout *main = new QVBoxLayout(ctx->win);

	/* 顶部：状态 + 操作按钮 */
	QHBoxLayout *top = new QHBoxLayout();
	ctx->statusLabel = new QLabel(ctx->win);
	ctx->statusLabel->setStyleSheet("color:#ccc;");
	ctx->switchBtn = new QPushButton("切换", ctx->win);
	ctx->switchBtn->setEnabled(false);
	ctx->refreshBtn = new QPushButton("刷新", ctx->win);
	top->addWidget(ctx->statusLabel);
	top->addStretch();
	top->addWidget(ctx->refreshBtn);
	top->addWidget(ctx->switchBtn);
	main->addLayout(top);

	/* 中间：可滚动网格 */
	ctx->scroll = new QScrollArea(ctx->win);
	ctx->gridContainer = new QWidget();
	ctx->grid = new QGridLayout(ctx->gridContainer);
	ctx->grid->setSpacing(6);
	ctx->scroll->setWidget(ctx->gridContainer);
	ctx->scroll->setWidgetResizable(true);
	main->addWidget(ctx->scroll);

	QObject::connect(ctx->switchBtn, &QPushButton::clicked, on_switch_clicked);
	QObject::connect(ctx->refreshBtn, &QPushButton::clicked,
			 [ctx]() { refresh_scene_list(ctx); });

	refresh_scene_list(ctx);

	ctx->timer = new QTimer(ctx->win);
	QObject::connect(ctx->timer, &QTimer::timeout, on_timer_tick);
	ctx->timer->start(300);

	ctx->win->show();
}

static void close_window(multiview_ctx *ctx)
{
	if (!ctx->winOpen)
		return;
	ctx->winOpen = false;
	if (ctx->timer) {
		ctx->timer->stop();
		ctx->timer->deleteLater();
		ctx->timer = nullptr;
	}
	/* 释放纹理（graphics 线程）与缓冲 */
	obs_queue_task(OBS_TASK_GRAPHICS, release_tiles_task, ctx, true);
	if (ctx->selected)
		ctx->selected = nullptr;
	if (ctx->win) {
		ctx->win->close();
		ctx->win->deleteLater();
		ctx->win = nullptr;
	}
}

/* ---------------------- 插件入口（由主插件调用） ---------------------- */

static void open_multiview(void *data)
{
	multiview_ctx *ctx = (multiview_ctx *)data;
	open_window(ctx);
}

bool multiview_module_load(void)
{
	g_ctx = new multiview_ctx();
	obs_frontend_add_tools_menu_item("多视图场景切换（动态网格）", open_multiview,
					 g_ctx);
	return true;
}

void multiview_module_unload(void)
{
	if (g_ctx) {
		close_window(g_ctx);
		delete g_ctx;
		g_ctx = nullptr;
	}
}

/* 让 moc 能处理 Q_OBJECT 子类（本文件内联启用） */
#include "obs-scene-multiview.moc"
