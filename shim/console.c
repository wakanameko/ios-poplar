/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

// フレームバッファ検出コードで使う定数の宣言
/* Poplar DT: mdss_mdp@c900000; vig-off 5000/7000/9000/b000, dma-off 25000/27000/29000/2b000 */
#define MDP_BASE	0x0c900000UL
#define SSPP_SRC_SIZE		0x00	/* [31:16] height, [15:0] width */
#define SSPP_SRC_IMG_SIZE	0x04
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24	/* [15:0] plane 0 stride in bytes */
#define SSPP_SRC_FORMAT		0x30	/* [10:9] bytes per pixel - 1 */
#define SSPP_SRC_UNPACK_PATTERN	0x34	/* byte i = channel of memory byte i: 0=G 1=B 2=R 3=A */
static const uint32_t pipe_off[8] = {
	0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000,
};

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_bg;
static int row;
// フレームバッファ検出コードで使う変数
int con_w = FB_W, con_h = FB_H, con_stride = FB_STRIDE, con_bpp = FB_BPP;
uint32_t con_pat = FB_PAT;		// B,G,R,A in memory = 0xAARRGGBB little-endian
struct con_pipe con_pipes[8];
int con_pipe_sel = -1;

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
int con_autodetect(void)
{
	int sel = -1;
 
	for (int i = 0; i < 8; i++) {
		uintptr_t p = MDP_BASE + pipe_off[i];
		struct con_pipe *c = &con_pipes[i];
		c->addr = rd32(p + SSPP_SRC0_ADDR);
		c->size = rd32(p + SSPP_SRC_SIZE);
		c->imgsize = rd32(p + SSPP_SRC_IMG_SIZE);
		c->stride = rd32(p + SSPP_SRC_YSTRIDE0) & 0xffff;
		c->fmt = rd32(p + SSPP_SRC_FORMAT);
		c->pat = rd32(p + SSPP_SRC_UNPACK_PATTERN);
		if (!c->addr || !(c->size & 0xffff) || !(c->size >> 16) || !c->stride)
			continue;
		if (c->addr >= FB_BASE && c->addr < FB_BASE + 0x2400000UL) {
			sel = i;	/* the splash buffer itself */
			break;
		}
		if (sel < 0)
			sel = i;
	}
	con_pipe_sel = sel;
	if (sel < 0)
		return -1;
	struct con_pipe *c = &con_pipes[sel];
	int bpp = ((c->fmt >> 9) & 3) + 1;
	if (bpp != 3 && bpp != 4)
		return -1;	/* keep the defaults */
	con_fb = (volatile uint32_t *)(uintptr_t)c->addr;
	con_w = c->size & 0xffff;
	con_h = c->size >> 16;
	con_stride = c->stride;
	con_bpp = bpp;
	con_pat = c->pat;
	return sel;
}
 
/* Convert 0xAARRGGBB to the bytes the display fetches, per the unpack pattern. */
static uint32_t conv(uint32_t c)
{
	uint32_t ch[4] = { (c >> 8) & 0xff, c & 0xff, (c >> 16) & 0xff, c >> 24 };
	uint32_t out = 0;
 
	for (int i = 0; i < 4; i++)
		out |= ch[(con_pat >> (8 * i)) & 3] << (8 * i);
	return out;
}
 
static inline void put_px(int x, int y, uint32_t raw)
{
	volatile uint8_t *line = (volatile uint8_t *)con_fb + (uintptr_t)y * con_stride;
 
	if (con_bpp == 4) {
		*(volatile uint32_t *)(line + x * 4) = raw;
	} else {
		volatile uint8_t *p = line + x * 3;
		p[0] = raw;
		p[1] = raw >> 8;
		p[2] = raw >> 16;
	}
}	// ここまでFB関連

void con_clear(uint32_t bg)
{
	uint32_t raw = conv(bg);

	for (int y = 0; y < con_h; y++) {
		for (int x = 0; x < con_w; x++)
			put_px(x, y, raw);
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	uint32_t raw_fg = conv(fg), raw_bg = conv(con_bg);
	if (x0 + CH_W > con_w || y0 + CH_H > con_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? raw_fg : raw_bg;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, c);
		}
}
 
static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}
 
static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}
 
void con_skip(void)
{
	row++;
}
 
void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}
 
void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}
 
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}
