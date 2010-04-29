#include <common.h>
#include <asm/io.h>
#include <hush.h>
#include <malloc.h>
#include <lcd.h>
#include <twl4030.h>

/* game buttons as in GPIO bank 4 */
#define BTN_R		GPIO9
#define BTN_UP		GPIO14
#define BTN_DOWN	GPIO7
#define BTN_G2		GPIO15
#define BTN_G3		GPIO10

struct menu_item {
	const char *name;
	int (*handler)(struct menu_item *item);
	char *cmd;
};

static struct menu_item *menu_items[16];
static int menu_item_count;

static int do_cmd(const char *fmt, ...)
{
	char cmdbuff[256];
	va_list args;

	va_start(args, fmt);
	vsprintf(cmdbuff, fmt, args);
	va_end(args);

	return !parse_string_outer(cmdbuff,
		FLAG_PARSE_SEMICOLON | FLAG_EXIT_FROM_LOOP);
}

static u32 menu_wait_for_input(int down)
{
	struct gpio *gpio4_base = (struct gpio *)OMAP34XX_GPIO4_BASE;
	u32 btns;

	while (1) {
		btns = ~readl(&gpio4_base->datain) &
			(BTN_UP|BTN_DOWN|BTN_G2|BTN_G3);
		if (!btns == !down)
			break;
		udelay(5000);
	}

	return btns;
}

static int menu_do_default(struct menu_item *item)
{
	return 1;
}

static int menu_do_poweroff(struct menu_item *item)
{
	u8 d;

	printf("power off.\n");

	twl4030_i2c_read_u8(TWL4030_CHIP_PM_MASTER, &d, TWL4030_PM_MASTER_P1_SW_EVENTS);
	d |= TWL4030_PM_MASTER_SW_EVENTS_DEVOFF;
	twl4030_i2c_write_u8(TWL4030_CHIP_PM_MASTER, d, TWL4030_PM_MASTER_P1_SW_EVENTS);

	return 0;
}

static int menu_do_script_cmd(struct menu_item *item)
{
	int failed = 0;

	if (item->cmd == NULL || !do_cmd(item->cmd))
		failed = 1;

	printf("script %s.\n", failed ? "failed" : "finished");
	menu_wait_for_input(0);
	menu_wait_for_input(1);
	return 0;
}

static struct menu_item default_menu_items[] = {
	{ "default boot",	menu_do_default, },
	{ "power off",		menu_do_poweroff, },
};

static void menu_init(void)
{
	const char *check_format1 = "%sload mmc1 0:%d 0x82000000 boot.scr";
	const char *check_format2 = "%sload mmc1 0:%d 0x82000000 boot.txt";
	const char *run_format1 = "%sload mmc1 0:%d 0x82000000 boot.scr;source 0x82000000";
	const char *run_format2 = "mw.l 0x82000000 0 1024;%sload mmc1 0:%d 0x82000000 boot.txt;"
					"ssource 0x82000000";
	disk_partition_t part_info;
	block_dev_desc_t *dev_desc;
	char tmp_name[32], tmp_cmd[128];
	struct menu_item *mitem;
	int i;

	for (i = 0; i < ARRAY_SIZE(default_menu_items); i++)
		menu_items[i] = &default_menu_items[i];
	menu_item_count = i;

	if (!do_cmd("mmc init"))
		return;

	dev_desc = get_dev("mmc1", 0);
	if (dev_desc == NULL) {
		printf("dev desc null\n");
		return;
	}

	/* kill stdout while we search for bootfiles */
	setenv("stdout", "nulldev");

	for (i = 1; menu_item_count < ARRAY_SIZE(menu_items); i++) {
		if (get_partition_info(dev_desc, i, &part_info))
			break;
		if (do_cmd("fatls mmc1 0:%d", i)) {
			if (do_cmd(check_format1, "fat", i)) {
				sprintf(tmp_cmd, run_format1, "fat", i);
				goto found;
			}
			if (do_cmd(check_format2, "fat", i)) {
				sprintf(tmp_cmd, run_format2, "fat", i);
				goto found;
			}
			continue;
		}
		if (do_cmd("ext2ls mmc1 0:%d", i)) {
			if (do_cmd(check_format1, "ext2", i)) {
				sprintf(tmp_cmd, run_format1, "ext2", i);
				goto found;
			}
			if (do_cmd(check_format2, "ext2", i)) {
				sprintf(tmp_cmd, run_format2, "ext2", i);
				goto found;
			}
			continue;
		}
		continue;

found:
		mitem = malloc(sizeof(*mitem));
		if (mitem == NULL)
			break;
		sprintf(tmp_name, "boot from SD1:%d", i);
		mitem->name = strdup(tmp_name);
		mitem->handler = menu_do_script_cmd;
		mitem->cmd = strdup(tmp_cmd);
		menu_items[menu_item_count++] = mitem;
	}

	setenv("stdout", "lcd");
}

static int boot_menu(cmd_tbl_t *cmdtp, int flag, int argc, char *argv[])
{
	int tl_row = panel_info.vl_row / 16 / 2 - (menu_item_count + 2) / 2;
	int i, sel = 0, max_sel;
	u32 btns;

	menu_init();

	max_sel = menu_item_count - 1;

	console_col = 3;
	console_row = tl_row;
	lcd_printf("Boot menu");

	while (1)
	{
		for (i = 0; i < menu_item_count; i++) {
			console_col = 3;
			console_row = tl_row + 2 + i;
			lcd_printf(menu_items[i]->name);
		}

		for (i = 0; i < menu_item_count; i++) {
			console_col = 1;
			console_row = tl_row + 2 + i;
			lcd_printf(i == sel ? ">" : " ");
		}

		menu_wait_for_input(0);
		btns = menu_wait_for_input(1);
		if (btns & BTN_UP) {
			sel--;
			if (sel < 0)
				sel = max_sel;
		}
		else if (btns & BTN_DOWN) {
			sel++;
			if (sel > max_sel)
				sel = 0;
		}
		else {
			do_cmd("cls");
			if (menu_items[sel]->handler(menu_items[sel]))
				break;
			do_cmd("cls");
		}
	}

	return 0;
}

U_BOOT_CMD(
	pmenu, 1, 1, boot_menu,
	"show pandora's boot menu",
	""
);

/* helper */
static int do_ssource(cmd_tbl_t *cmdtp, int flag, int argc, char *argv[])
{
	ulong addr;

	if (argc < 2)
		return 1;

	addr = simple_strtoul(argv[1], NULL, 16);

	printf("## Executing script at %08lx\n", addr);
	return parse_string_outer((char *)addr,
		FLAG_PARSE_SEMICOLON | FLAG_EXIT_FROM_LOOP);
}

U_BOOT_CMD(
	ssource, 2, 0, do_ssource,
	"run script from memory (no header)",
	"<addr>"
);
