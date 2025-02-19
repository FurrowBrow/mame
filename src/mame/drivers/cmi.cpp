// license:BSD-3-Clause
// copyright-holders:Phil Bennett
/***************************************************************************

    Fairlight CMI Series

    driver by Phil Bennett

    Systems supported:

        * CMI IIx

    To do:
    * V12 system software reports that it can't load MIDI support and then hangs.

    Information from:

             CMI SYSTEM SERVICE MANUAL
        FAIRLIGHT INSTRUMENTS, FEBRUARY 1985
                  Revision 2.1

    This document is available on archive.org at the following URL:

    https://archive.org/details/fairlight_CMI-IIx_SERVICE_MANUAL

    Summary:

        The Fairlight CMI system conists typically of:
            - One velocity-sensitive unweighted keyboard, with a numeric
              keypad and several control surfaces
            - (Optionally) one additional keyboard, not velocity-sensitive
            - One alphanumeric keyboard for manual control
            - A 15-inch green-screen monitor and light pen for more direct
              control
            - A box consisting of:
              * An audio board including balanced line drivers for eight
                channels and mixed output
              * A 500-watt power supply
              * A 21-slot backplane
              * Two 8-inch double-density floppy disk drives. The format
                used is soft-sectored, 128 bytes per sector (single density),
                or 256 bytes per sector (double density), using FM
                recording.
              * And the following cards:
                Slot  1: Master Card CMI-02
                Slot  2: General Interface Card CMI-08/28 (Optional)
                Slots 3-11: 8 Channel Controller Cards & 1 Voice Master Module Card, order unknown
                Slot 12: 64K System RAM Q-096
                Slot 13: 256K System RAM Q-256
                Slot 14: 256K System RAM Q-256
                Slot 15: 4-Port ACIA Module Q-014 (Optional)
                Slot 16: Processor Control Module Q-133
                Slot 17: Central Processor Module Q-209
                Slot 18: Lightpen/Graphics Interface Q-219
                Slot 19: Floppy Disk Controller QFC-9
                Slot 20: Hard Disk Controller Q-077 (Optional)

        Q209 Dual 6809 Central Processor Card
        -------------------------------------

        The CPU card has two 6809 processors, with robust inter-CPU
        communications capabilities including:
            - Uninterruptible instructions
            - CPU-specific ID register and memory map registers
            - Interprocessor interrupts
            - Automatic memory map-switching register

        The CPUs are multiplexed onto the address and data buses
        in an interleaved manner such that there is no contention
        on simultaneous memory accesses.

        All system timing is derived from a 40MHz clock crystal, which
        is divided into two opposite-phase 20MHz squre waves.

        Other data entry from service manual to be completed later - RH 12 Aug 2016

****************************************************************************/

#include "emu.h"

#include "audio/cmi01a.h"
#include "machine/cmi_ankbd.h"
#include "machine/cmi_mkbd.h"

#include "bus/midi/midi.h"

#include "cpu/m6809/m6809.h"
#include "cpu/m68000/m68000.h"

#include "imagedev/floppy.h"
#include "machine/6821pia.h"
#include "machine/6850acia.h"
#include "machine/6840ptm.h"
#include "machine/7474.h"
#include "machine/bankdev.h"
#include "machine/clock.h"
#include "machine/i8214.h"
#include "machine/input_merger.h"
#include "machine/mos6551.h"
#include "machine/msm5832.h"
#include "machine/wd_fdc.h"

#include "emupal.h"
#include "screen.h"
#include "speaker.h"

#define VERBOSE     (0)
#include "logmacro.h"

#define Q209_CPU_CLOCK          (40.21_MHz_XTAL / 40) // verified by manual
#define SYSTEM_CAS_CLOCK        (40.21_MHz_XTAL / 20) // likewise

#define M6809_CLOCK             8000000 // wrong
#define MASTER_OSCILLATOR       34.291712_MHz_XTAL

#define CPU_1                   0
#define CPU_2                   1

#define MAPPING_A               1
#define MAPPING_B               0

#define NUM_Q256_CARDS          1   // Max of 2
#define NUM_CHANNEL_CARDS       8

#define PAGE_SIZE               2048
#define PAGE_COUNT              (65536 / PAGE_SIZE)
#define PAGE_MASK               (PAGE_SIZE - 1)
#define PAGE_SHIFT              5

#define PIXEL_CLOCK             10.38_MHz_XTAL
#define HTOTAL                  672
#define HBLANK_END              0
#define HBLANK_START            512
#define VTOTAL                  304
#define VBLANK_END              0
#define VBLANK_START            256

#define HBLANK_FREQ     (PIXEL_CLOCK / HTOTAL)
#define VBLANK_FREQ     (HBLANK_FREQ / VTOTAL)

#define MAPSEL_P2_B             0x00
#define MAPSEL_P2_A             0x03
#define MAPSEL_P2_A_DMA1        0x04
#define MAPSEL_P2_A_DMA2        0x05
#define MAPSEL_P2_A_DMA3        0x06
#define MAPSEL_P2_A_DMA4        0x07
#define MAPSEL_P1_B             0x08
#define MAPSEL_P1_A             0x0b
#define MAPSEL_P1_A_DMA1        0x0c
#define MAPSEL_P1_A_DMA2        0x0d
#define MAPSEL_P1_A_DMA3        0x0e
#define MAPSEL_P1_A_DMA4        0x0f

#define IRQ_ACINT_LEVEL         (0 ^ 7)
#define IRQ_MIDINT_LEVEL        (0 ^ 7)
#define IRQ_TIMINT_LEVEL        (1 ^ 7)
#define IRQ_INTP1_LEVEL         (2 ^ 7)
#define IRQ_IPI1_LEVEL          (3 ^ 7)
#define IRQ_SMIDINT_LEVEL       (3 ^ 7)
#define IRQ_AIC_LEVEL           (4 ^ 7)

static const int ch_int_levels[8] =
{
	12 ^ 7, 8 ^ 7, 13 ^ 7, 9 ^ 7, 14 ^ 7, 10 ^ 7, 15 ^ 7, 11  ^ 7
};

#define IRQ_PERRINT_LEVEL       (0 ^ 7)
#define IRQ_RTCINT_LEVEL        (0 ^ 7)
#define IRQ_RINT_LEVEL          (1 ^ 7)
#define IRQ_INTP2_LEVEL         (2 ^ 7)
#define IRQ_IPI2_LEVEL          (3 ^ 7)
#define IRQ_TOUCHINT_LEVEL      (4 ^ 7)
#define IRQ_PENINT_LEVEL        (5 ^ 7)
#define IRQ_ADINT_LEVEL         (6 ^ 7)
#define IRQ_DISKINT_LEVEL       (7 ^ 7)

#define FDC_CONTROL_INTEN       (1 << 2)

#define FDC_STATUS_READY        (1 << 3)
#define FDC_STATUS_TWO_SIDED    (1 << 4)
#define FDC_STATUS_DISK_CHANGE  (1 << 5)
#define FDC_STATUS_INTERRUPT    (1 << 6)
#define FDC_STATUS_DRIVER_LOAD  (1 << 7)

class cmi_state : public driver_device
{
public:
	cmi_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_maincpu1(*this, "maincpu1")
		, m_maincpu2(*this, "maincpu2")
		, m_midicpu(*this, "smptemidi")
		, m_cmi07cpu(*this, "cmi07cpu")
		, m_maincpu1_irq_merger(*this, "maincpu1_irq_merger")
		, m_maincpu2_irq0_merger(*this, "maincpu2_irq0_merger")
		, m_msm5832(*this, "msm5832")
		, m_i8214(*this, "i8214_%u", 1U)
		, m_q133_pia(*this, "q133_pia_%u", 1U)
		, m_q133_ptm(*this, "q133_ptm")
		, m_q133_acia(*this, "q133_acia_%u", 0U)
		, m_q133_region(*this, "q133")
		, m_q219_pia(*this, "q219_pia")
		, m_q219_ptm(*this, "q219_ptm")
		, m_cmi02_pia(*this, "cmi02_pia_%u", 1U)
		, m_cmi02_ptm(*this, "cmi02_ptm")
		, m_cmi07_ptm(*this, "cmi07_ptm")
		, m_midi_ptm(*this, "midi_ptm_%u", 1U)
		, m_midi_acia(*this, "midi_acia_%u", 1U)
		, m_midi_out(*this, "midi_out_%u", 1U)
		, m_midi_in(*this, "midi_in_%u", 1U)
		, m_qfc9_region(*this, "qfc9")
		, m_floppy0(*this, "wd1791:0")
		, m_floppy1(*this, "wd1791:1")
		, m_wd1791(*this, "wd1791")
		, m_channels(*this, "cmi01a_%u", 0)
		, m_screen(*this, "screen")
		, m_palette(*this, "palette")
		, m_lp_x_port(*this, "LP_X")
		, m_lp_y_port(*this, "LP_Y")
		, m_lp_touch_port(*this, "LP_TOUCH")
		, m_cmi07_ram(*this, "cmi07_ram")
		, m_cpu1_periphs(*this, "cpu1_periphs")
		, m_cpu2_periphs(*this, "cpu2_periphs")
	{
	}

	void set_interrupt(int cpunum, int level, int state);

	virtual void machine_reset() override;
	virtual void machine_start() override;
	virtual void device_timer(emu_timer &timer, device_timer_id id, int param) override;

	static const device_timer_id TIMER_MAP_SWITCH = 0;
	static const device_timer_id TIMER_HBLANK = 1;
	static const device_timer_id TIMER_JAM_TIMEOUT = 2;

	void init_cmi2x();

	// CPU card
	DECLARE_WRITE_LINE_MEMBER( q133_acia_irq );
	DECLARE_WRITE_LINE_MEMBER( q133_ptm_irq_w );
	void i8214_cpu1_w(uint8_t data);
	void i8214_cpu2_w(uint8_t data);
	DECLARE_WRITE_LINE_MEMBER( maincpu1_irq_w );
	DECLARE_WRITE_LINE_MEMBER( maincpu2_irq0_w );
	DECLARE_WRITE_LINE_MEMBER( i8214_1_int_w );
	DECLARE_WRITE_LINE_MEMBER( i8214_2_int_w );
	DECLARE_WRITE_LINE_MEMBER( i8214_3_int_w );
	DECLARE_WRITE_LINE_MEMBER( i8214_3_enlg );
	uint8_t shared_ram_r(offs_t offset);
	void shared_ram_w(offs_t offset, uint8_t data);
	template<int cpunum> uint8_t perr_r(offs_t offset);
	template<int cpunum> void perr_w(offs_t offset, uint8_t data);

	uint16_t m_aic_ad565_in[16]{};
	uint8_t m_aic_mux_latch = 0;

	uint8_t aic_ad574_r();
	template<int Dac> void aic_dac_w(uint8_t data);
	void aic_mux_latch_w(uint8_t data);
	void aic_ad565_msb_w(uint8_t data);
	void aic_ad565_lsb_w(uint8_t data);

	uint8_t q133_1_porta_r();
	void q133_1_porta_w(uint8_t data);
	void q133_1_portb_w(uint8_t data);

	DECLARE_WRITE_LINE_MEMBER( cmi_iix_vblank );
	IRQ_CALLBACK_MEMBER( cpu1_interrupt_callback );
	IRQ_CALLBACK_MEMBER( cpu2_interrupt_callback );

	// Video-related
	uint8_t video_r(offs_t offset);
	uint8_t lightpen_r(offs_t offset);
	uint8_t pia_q219_b_r();
	void video_w(offs_t offset, uint8_t data);
	void vscroll_w(uint8_t data);
	void video_attr_w(uint8_t data);

	uint8_t vram_r(offs_t offset);
	void vram_w(offs_t offset, uint8_t data);

	template <int cpunum, uint16_t base> uint8_t ram_range_r(offs_t offset);
	template <int cpunum, uint16_t base> void ram_range_w(offs_t offset, uint8_t data);
	template <int cpunum> uint8_t vram_range_r(offs_t offset);
	template <int cpunum> void vram_range_w(offs_t offset, uint8_t data);
	template <int cpunum> uint8_t cards_range_r(offs_t offset);
	template <int cpunum> void cards_range_w(offs_t offset, uint8_t data);
	template <int cpunum> uint8_t periphs_range_r(offs_t offset);
	template <int cpunum> void periphs_range_w(offs_t offset, uint8_t data);

	uint8_t tvt_r();
	void tvt_w(uint8_t data);
	DECLARE_WRITE_LINE_MEMBER( pia_q219_irqa );
	DECLARE_WRITE_LINE_MEMBER( pia_q219_irqb );
	DECLARE_WRITE_LINE_MEMBER( ptm_q219_irq );
	uint32_t screen_update_cmi2x(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);

	// Memory mapping
	template<int cpunum> uint8_t rom_r(offs_t offset);
	void map_ram_w(offs_t offset, uint8_t data);
	template<int cpunum> uint8_t vector_r(offs_t offset);
	template<int cpunum> uint8_t map_r();
	template<int cpunum> void map_w(uint8_t data);
	uint8_t atomic_r();
	void cpufunc_w(uint8_t data);
	uint8_t parity_r(offs_t offset);
	void mapsel_w(offs_t offset, uint8_t data);
	template<int cpunum> uint8_t irq_ram_r(offs_t offset);
	template<int cpunum> void irq_ram_w(offs_t offset, uint8_t data);
	template<int cpunum> uint8_t scratch_ram_r(offs_t offset);
	template<int cpunum> void scratch_ram_w(offs_t offset, uint8_t data);
	template<int cpunum> uint8_t scratch_ram_fa_r(offs_t offset);
	template<int cpunum> void scratch_ram_fa_w(offs_t offset, uint8_t data);

	// MIDI/SMPTE
	void midi_dma_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	uint16_t midi_dma_r(offs_t offset);
	DECLARE_WRITE_LINE_MEMBER( midi_ptm0_c3_w );
	DECLARE_WRITE_LINE_MEMBER( midi_ptm_irq );
	DECLARE_WRITE_LINE_MEMBER( midi_acia_irq );
	void midi_latch_w(uint8_t data);

	// Floppy
	void fdc_w(offs_t offset, uint8_t data);
	uint8_t fdc_r(offs_t offset);
	DECLARE_WRITE_LINE_MEMBER( wd1791_irq );
	DECLARE_WRITE_LINE_MEMBER( wd1791_drq );

	// Master card
	uint8_t cmi02_r(offs_t offset);
	void cmi02_w(offs_t offset, uint8_t data);
	void cmi02_chsel_w(uint8_t data);
	void master_tune_w(uint8_t data);
	DECLARE_WRITE_LINE_MEMBER( cmi02_ptm_irq );
	DECLARE_WRITE_LINE_MEMBER( cmi02_ptm_o2 );
	DECLARE_WRITE_LINE_MEMBER( cmi02_pia2_irqa_w );
	DECLARE_WRITE_LINE_MEMBER( cmi02_pia2_cb2_w );

	uint8_t cmi07_r();
	void cmi07_w(uint8_t data);

	DECLARE_WRITE_LINE_MEMBER( msm5832_irq_w );
	DECLARE_WRITE_LINE_MEMBER( cmi07_irq );
	DECLARE_WRITE_LINE_MEMBER( q133_acia_clock );

	template<int Channel> DECLARE_WRITE_LINE_MEMBER( channel_irq );

	void cmi2x(machine_config &config);
	void cmi07cpu_map(address_map &map);
	void maincpu1_map(address_map &map);
	void maincpu2_map(address_map &map);
	void midicpu_map(address_map &map);

	void cpu1_periphs_map(address_map &map);
	void cpu2_periphs_map(address_map &map);

protected:
	required_device<mc6809e_device> m_maincpu1;
	required_device<mc6809e_device> m_maincpu2;
	required_device<m68000_device> m_midicpu;
	required_device<mc6809e_device> m_cmi07cpu;

	required_device<input_merger_any_high_device> m_maincpu1_irq_merger;
	required_device<input_merger_any_high_device> m_maincpu2_irq0_merger;
	required_device<msm5832_device> m_msm5832;
	required_device_array<i8214_device, 3> m_i8214;
	required_device_array<pia6821_device, 2> m_q133_pia;
	required_device<ptm6840_device> m_q133_ptm;
	required_device_array<mos6551_device, 4> m_q133_acia;
	required_memory_region m_q133_region;

	required_device<pia6821_device> m_q219_pia;
	required_device<ptm6840_device> m_q219_ptm;

	required_device_array<pia6821_device, 2> m_cmi02_pia;
	required_device<ptm6840_device> m_cmi02_ptm;

	required_device<ptm6840_device> m_cmi07_ptm;

	required_device_array<ptm6840_device, 2> m_midi_ptm;
	required_device_array<acia6850_device, 4> m_midi_acia;
	required_device_array<midi_port_device, 4> m_midi_out;
	required_device_array<midi_port_device, 3> m_midi_in;

	required_memory_region m_qfc9_region;
	required_device<floppy_connector> m_floppy0;
	required_device<floppy_connector> m_floppy1;
	required_device<fd1791_device> m_wd1791;

	required_device_array<cmi01a_device, 8> m_channels;

	required_device<screen_device> m_screen;
	required_device<palette_device> m_palette;

	required_ioport m_lp_x_port;
	required_ioport m_lp_y_port;
	required_ioport m_lp_touch_port;

	required_shared_ptr<uint8_t> m_cmi07_ram;

	required_device<address_map_bank_device> m_cpu1_periphs;
	required_device<address_map_bank_device> m_cpu2_periphs;

	address_space *m_cpu1space = nullptr;
	address_space *m_cpu2space = nullptr;

private:
	emu_timer *m_map_switch_timer = nullptr;
	emu_timer *m_hblank_timer = nullptr;
	emu_timer *m_jam_timeout_timer = nullptr;

	uint8_t m_video_data = 0;

	// Memory
	bool map_is_active(int cpunum, int map, uint8_t *map_info);
	void update_address_space(int cpunum, uint8_t mapinfo);

	// Video
	void hblank();
	template <int Y, int X, bool ByteSize> void update_video_pos();

	// Floppy
	void dma_fdc_rom();
	void write_fdc_ctrl(uint8_t data);
	void fdc_dma_transfer();

	// Q133 CPU Card
	uint8_t *m_q133_rom = nullptr;

	uint16_t  m_int_state[2]{};
	uint8_t   m_lp_int = 0;
	uint8_t   m_hp_int = 0;
	std::unique_ptr<uint8_t[]>    m_shared_ram;
	std::unique_ptr<uint8_t[]>    m_scratch_ram[2];

	/* Memory management */
	uint8_t   m_map_sel[16]{};
	std::unique_ptr<uint8_t[]>    m_map_ram[2];
	std::unique_ptr<uint8_t[]>    m_q256_ram[2];
	uint8_t   m_map_ram_latch = 0;
	int     m_cpu_active_space[2]{};
	int     m_cpu_map_switch[2]{};
	uint8_t m_curr_mapinfo[2]{};
	uint8_t m_irq_address[2][2]{};
	int     m_m6809_bs_hack_cnt[2]{};

	/* Q219 lightpen/graphics card */
	std::unique_ptr<uint8_t[]>    m_video_ram;
	uint16_t  m_x_pos = 0;
	uint8_t   m_y_pos = 0;
	uint16_t  m_lp_x = 0;
	uint8_t   m_lp_y = 0;
	uint8_t   m_q219_b_touch = 0;

	/* QFC9 floppy disk controller card */
	uint8_t * m_qfc9_region_ptr = 0;
	int       m_fdc_drq = 0;
	uint8_t   m_fdc_addr = 0;
	uint8_t   m_fdc_ctrl = 0;
	uint8_t   m_fdc_status = 0;
	PAIR      m_fdc_dma_addr{};
	PAIR      m_fdc_dma_cnt{};

	/* CMI-07 */
	uint8_t   m_cmi07_ctrl = 0;
	bool      m_cmi07_base_enable[2]{};
	uint16_t  m_cmi07_base_addr = 0;

	uint8_t   m_msm5832_addr = 0;

	// Master card (CMI-02)
	int       m_cmi02_ptm_irq = 0;
	uint8_t   m_cmi02_pia_chsel = 0;
};

/**************************************
 *
 *  Video hardware
 *
 *************************************/

uint32_t cmi_state::screen_update_cmi2x(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	const pen_t *pen = m_palette->pens();
	uint8_t y_scroll = m_q219_pia->a_output();
	uint8_t invert = BIT(~m_q219_pia->b_output(), 3);

	for (int y = cliprect.min_y; y <= cliprect.max_y; ++y)
	{
		uint8_t *src = &m_video_ram[(512/8) * ((y + y_scroll) & 0xff)];
		uint32_t *dest = &bitmap.pix(y, cliprect.min_x);

		for (int x = cliprect.min_x; x <= cliprect.max_x; x += 8)
		{
			uint8_t data = *src++;

			/* Store 8 pixels */
			for (int i = 0; i < 8; ++i)
				*dest++ = pen[BIT(data, 7 - i) ^ invert];
		}
	}

	/* Get lightpen position */
	//if (LPCEN && NOT_TOUCHING)
	if (m_lp_touch_port->read() && BIT(m_q219_pia->b_output(), 1))
	{
		/* Invert target pixel */
		bitmap.pix(m_lp_y_port->read(), m_lp_x_port->read()) ^= 0x00ffffff;
	}



	return 0;
}

void cmi_state::hblank()
{
	int v = m_screen->vpos();

	if (!m_screen->vblank())
	{
		int _touch = m_lp_touch_port->read();
		int _tfh = !BIT(m_q219_pia->b_output(), 2);

		if (v == m_lp_y_port->read())
		{
			m_q219_b_touch = _touch ? 0 : (1 << 5);
			m_q219_pia->ca1_w(_touch ? 0 : 1);

			if (!_touch || !_tfh)
			{
				/* Latch the counters */
				m_lp_x = m_lp_x_port->read();
				m_lp_y = m_lp_y_port->read();

				/* LPSTB */
				m_q219_pia->cb1_w(1);
			}
		}
	}
	/* Adjust for next scanline */
	if (++v >= VTOTAL)
		v = 0;

	m_hblank_timer->adjust(m_screen->time_until_pos(v, HBLANK_START));

}

template void cmi_state::update_video_pos< 0,  0, false>();
template void cmi_state::update_video_pos< 0,  1, false>();
template void cmi_state::update_video_pos< 0, -1, false>();
template void cmi_state::update_video_pos< 0,  0, true>();
template void cmi_state::update_video_pos< 1,  0, false>();
template void cmi_state::update_video_pos< 1,  1, false>();
template void cmi_state::update_video_pos< 1, -1, false>();
template void cmi_state::update_video_pos< 1,  0, true>();
template void cmi_state::update_video_pos<-1,  0, false>();
template void cmi_state::update_video_pos<-1,  1, false>();
template void cmi_state::update_video_pos<-1, -1, false>();
template void cmi_state::update_video_pos<-1,  0, true>();

template <int Y, int X, bool ByteSize> void cmi_state::update_video_pos()
{
	uint8_t *video_addr = &m_video_ram[m_y_pos * (512 / 8) + (m_x_pos / 8)];

	if (ByteSize)
	{
		*video_addr = m_video_data;
	}
	else
	{
		int bit_mask = 1 << ((7 ^ m_x_pos) & 7);

		*video_addr &= ~bit_mask;
		*video_addr |= m_video_data & bit_mask;
	}

	m_y_pos = (m_y_pos + Y) & 0xff;
	m_x_pos = (m_x_pos + X) & 0x1ff;
}

uint8_t cmi_state::video_r(offs_t offset)
{
	if (machine().side_effects_disabled())
		return m_video_data;

	m_video_data = m_video_ram[m_y_pos * (512 / 8) + (m_x_pos / 8)];

	switch (offset & 0x0f)
	{
		case 0x0: update_video_pos< 0,  0, false>(); break;
		case 0x1: update_video_pos< 0,  1, false>(); break;
		case 0x2: update_video_pos< 0, -1, false>(); break;
		case 0x3: update_video_pos< 0,  0, true>(); break;
		case 0x4: update_video_pos< 1,  0, false>(); break;
		case 0x5: update_video_pos< 1,  1, false>(); break;
		case 0x6: update_video_pos< 1, -1, false>(); break;
		case 0x7: update_video_pos< 1,  0, true>(); break;
		case 0x8: update_video_pos<-1,  0, false>(); break;
		case 0x9: update_video_pos<-1,  1, false>(); break;
		case 0xa: update_video_pos<-1, -1, false>(); break;
		case 0xb: update_video_pos<-1,  0, true>(); break;
		default: break;
	}

	return m_video_data;
}

uint8_t cmi_state::lightpen_r(offs_t offset)
{
	if (offset & 2)
		return m_lp_y;
	else
		return m_lp_x >> 1;
}

uint8_t cmi_state::pia_q219_b_r()
{
	return ((m_lp_x << 7) & 0x80) | m_q219_b_touch;
}


void cmi_state::video_w(offs_t offset, uint8_t data)
{
	m_video_data = data;

	switch (offset & 0x0f)
	{
		case 0x0: update_video_pos< 0,  0, false>(); break;
		case 0x1: update_video_pos< 0,  1, false>(); break;
		case 0x2: update_video_pos< 0, -1, false>(); break;
		case 0x3: update_video_pos< 0,  0, true>(); break;
		case 0x4: update_video_pos< 1,  0, false>(); break;
		case 0x5: update_video_pos< 1,  1, false>(); break;
		case 0x6: update_video_pos< 1, -1, false>(); break;
		case 0x7: update_video_pos< 1,  0, true>(); break;
		case 0x8: update_video_pos<-1,  0, false>(); break;
		case 0x9: update_video_pos<-1,  1, false>(); break;
		case 0xa: update_video_pos<-1, -1, false>(); break;
		case 0xb: update_video_pos<-1,  0, true>(); break;
		default: break;
	}
}

void cmi_state::vscroll_w(uint8_t data)
{
	// TODO: Partial updates. Also, this should be done through a PIA
}

void cmi_state::video_attr_w(uint8_t data)
{
	// TODO
}

void cmi_state::tvt_w(uint8_t data)
{
	if ((data >= 0x20 && data <= 0x7e) || data == 0x0a || data == 0x0d)
	{
		osd_printf_debug("%c", data);
	}
}

uint8_t cmi_state::tvt_r()
{
	return 0;
}

void cmi_state::vram_w(offs_t offset, uint8_t data)
{
	m_video_ram[offset] = data;
}

uint8_t cmi_state::vram_r(offs_t offset)
{
	if (machine().side_effects_disabled())
		return m_video_ram[offset];

	/* Latch the current video position */
	m_y_pos = (offset >> 6) & 0xff;
	m_x_pos = (offset & 0x3f) << 3;

	return m_video_ram[offset];
}



/* Memory handling */

template<int cpunum> uint8_t cmi_state::rom_r(offs_t offset)
{
	uint16_t base = (cpunum ? 0x1000 : 0x2000);
	return *(((uint8_t *)m_q133_region->base()) + base + offset);
}

template<int cpunum> uint8_t cmi_state::perr_r(offs_t offset)
{
	m_maincpu2_irq0_merger->in_w<1>(1);

	const uint8_t page = offset >> 11;
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];
	const uint8_t data = m_q256_ram[0][(page_info & 0x7f) * PAGE_SIZE + (offset & 0x7ff)];
	return data;
}

template<int cpunum> void cmi_state::perr_w(offs_t offset, uint8_t data)
{
	const uint8_t page = offset >> 11;
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];
	m_q256_ram[0][(page_info & 0x7f) * PAGE_SIZE + (offset & 0x7ff)] = data;
}

template <int cpunum, uint16_t base> uint8_t cmi_state::ram_range_r(offs_t offset)
{
	const uint16_t addr = base + offset;
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	const bool perr_en = BIT(mapinfo, 6);
	const uint8_t page = addr >> 11;
	const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

	if (m_cmi07_base_enable[cpunum] && (addr & 0xc000) == m_cmi07_base_addr)
	{
		return m_cmi07_ram[(page * PAGE_SIZE) & 0x3fff];
	}

	if (perr_en)
	{
		return perr_r<cpunum>(addr);
	}
	else if (BIT(page_info, 7))
	{
		const uint32_t ram_base = (page_info & 0x7f) << 11;
		return m_q256_ram[0][ram_base | (addr & 0x7ff)];
	}

	return 0x00;
}

template <int cpunum, uint16_t base> void cmi_state::ram_range_w(offs_t offset, uint8_t data)
{
	const uint16_t addr = base + offset;
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	const bool perr_en = BIT(mapinfo, 6);
	const uint8_t page = addr >> 11;
	const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

	if (m_cmi07_base_enable[cpunum] && (addr & 0xc000) == m_cmi07_base_addr)
	{
		m_cmi07_ram[(page * PAGE_SIZE) & 0x3fff] = data;
		return;
	}

	if (perr_en)
	{
		perr_w<cpunum>(addr, data);
	}
	else if (BIT(page_info, 7))
	{
		const uint32_t ram_base = (page_info & 0x7f) << 11;
		m_q256_ram[0][ram_base | (addr & 0x7ff)] = data;
	}
}

template <int cpunum> uint8_t cmi_state::vram_range_r(offs_t offset)
{
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	if (!BIT(mapinfo, 5))
	{
		return vram_r(offset);
	}
	else
	{
		const uint16_t address = 0x8000 + offset;
		const uint8_t page = (offset >> 11) + 16;
		const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

		if (m_cmi07_base_enable[cpunum] && (address & 0xc000) == m_cmi07_base_addr)
		{
			return m_cmi07_ram[(page * PAGE_SIZE) & 0x3fff];
		}

		if (BIT(page_info, 7))
		{
			const uint32_t ram_base = (page_info & 0x7f) << 11;
			return m_q256_ram[0][ram_base | (offset & 0x7ff)];
		}
	}

	return 0x00;
}

template <int cpunum> void cmi_state::vram_range_w(offs_t offset, uint8_t data)
{
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	if (!BIT(mapinfo, 5))
	{
		vram_w(offset, data);
	}
	else
	{
		const uint16_t address = 0x8000 + offset;
		const uint8_t page = (offset >> 11) + 16;
		const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

		if (m_cmi07_base_enable[cpunum] && (address & 0xc000) == m_cmi07_base_addr)
		{
			m_cmi07_ram[(page * PAGE_SIZE) & 0x3fff] = data;
			return;
		}

		if (BIT(page_info, 7))
		{
			const uint32_t ram_base = (page_info & 0x7f) << 11;
			m_q256_ram[0][ram_base | (offset & 0x7ff)] = data;
		}
	}
}

template <int cpunum> uint8_t cmi_state::cards_range_r(offs_t offset)
{
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	if (!BIT(mapinfo, 7) && offset < 0x40)
	{
		return cmi02_r(offset);
	}
	else
	{
		const uint8_t page = (offset >> 11) + 28;
		const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

		if (BIT(page_info, 7))
		{
			const uint32_t ram_base = (page_info & 0x7f) << 11;
			return m_q256_ram[0][ram_base | (offset & 0x7ff)];
		}
	}
	return 0x00;
}

template <int cpunum> void cmi_state::cards_range_w(offs_t offset, uint8_t data)
{
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	if (!BIT(mapinfo, 7) && offset < 0x40)
	{
		cmi02_w(offset, data);
	}
	else
	{
		const uint8_t page = (offset >> 11) + 28;
		const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

		if (BIT(page_info, 7))
		{
			const uint32_t ram_base = (page_info & 0x7f) << 11;
			m_q256_ram[0][ram_base | (offset & 0x7ff)] = data;
		}
	}
}

template <int cpunum> uint8_t cmi_state::periphs_range_r(offs_t offset)
{
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	if (!BIT(mapinfo, 7))
	{
		if (cpunum)
			return m_cpu2_periphs->read8(offset);
		else
			return m_cpu1_periphs->read8(offset);
	}
	else
	{
		const uint8_t page = (offset >> 11) + 30;
		const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

		if (BIT(page_info, 7))
		{
			const uint32_t ram_base = (page_info & 0x7f) << 11;
			return m_q256_ram[0][ram_base | (offset & 0x7ff)];
		}
	}

	return 0x00;
}

template <int cpunum> void cmi_state::periphs_range_w(offs_t offset, uint8_t data)
{
	const uint8_t mapinfo = m_curr_mapinfo[cpunum];
	if (!BIT(mapinfo, 7))
	{
		if (cpunum)
			m_cpu2_periphs->write8(offset, data);
		else
			m_cpu1_periphs->write8(offset, data);
	}
	else
	{
		const uint8_t page = (offset >> 11) + 30;
		const uint8_t page_info = m_map_ram[0][((mapinfo & 0x1f) << PAGE_SHIFT) + page];

		if (BIT(page_info, 7))
		{
			const uint32_t ram_base = (page_info & 0x7f) << 11;
			m_q256_ram[0][ram_base | (offset & 0x7ff)] = data;
		}
	}
}

void cmi_state::map_ram_w(offs_t offset, uint8_t data)
{
	if ((offset & 1) == 0)
	{
		m_map_ram_latch = data;
	}
	else
	{
		uint8_t map_info;
		int map = (offset >> 6);
		int page_enable = ((m_map_ram_latch & 0x80) && (0 == (m_map_ram_latch & 7))) ? 0x80 : 0;

		m_map_ram[0][offset >> 1] = page_enable | (data & 0x7f);

		/* Determine if this map is in use by either CPU */
		if (map_is_active(CPU_1, map, &map_info))
			update_address_space(0, map_info);

		if (map_is_active(CPU_2, map, &map_info))
			update_address_space(1, map_info);
	}
}

template<int cpunum> uint8_t cmi_state::vector_r(offs_t offset)
{
	return m_q133_rom[(cpunum ? 0xbfe : 0xffe) + offset];
}

template<int cpunum> uint8_t cmi_state::map_r()
{
	return (m_cpu_active_space[1] << 2) | (m_cpu_active_space[0] << 1) | cpunum;
}

template<int cpunum> void cmi_state::map_w(uint8_t data)
{
	m_map_switch_timer->adjust(attotime::from_ticks(data & 0xf, M6809_CLOCK), cpunum);
}

template<int cpunum> uint8_t cmi_state::irq_ram_r(offs_t offset)
{
	if (machine().side_effects_disabled())
		return m_scratch_ram[cpunum][0xf8 + offset];

	if (m_m6809_bs_hack_cnt[cpunum] > 0)
	{
		m_m6809_bs_hack_cnt[cpunum]--;
		LOG("CPU%d IRQ vector byte %d (offset %d): %02x\n", cpunum + 1, 1 - m_m6809_bs_hack_cnt[cpunum], offset, m_irq_address[cpunum][offset]);
		return m_irq_address[cpunum][offset];
	}
	return m_scratch_ram[cpunum][0xf8 + offset];
}

template<int cpunum> void cmi_state::irq_ram_w(offs_t offset, uint8_t data)
{
	m_scratch_ram[cpunum][0xf8 + offset] = data;
}

void cmi_state::device_timer(emu_timer &timer, device_timer_id id, int param)
{
	switch (id)
	{
		case TIMER_MAP_SWITCH:
		{
			m_cpu_active_space[param] = m_cpu_map_switch[param];
			uint8_t map_info = (m_cpu_map_switch[param] == MAPPING_A) ?
							 m_map_sel[param ? MAPSEL_P2_A : MAPSEL_P1_A] :
							 m_map_sel[param ? MAPSEL_P2_B : MAPSEL_P1_B];
			update_address_space(param, map_info);
			m_map_switch_timer->adjust(attotime::never);
			break;
		}

		case TIMER_HBLANK:
			hblank();
			break;

		case TIMER_JAM_TIMEOUT:
			m_maincpu2->set_input_line(INPUT_LINE_HALT, CLEAR_LINE);
			m_jam_timeout_timer->adjust(attotime::never);
			break;
	}
}

uint8_t cmi_state::atomic_r()
{
	// TODO
	//osd_printf_debug("atomic access\n");
	return 0;
}

void cmi_state::cpufunc_w(uint8_t data)
{
	int cpunum = data & 1;
	int idx = data & 6;
	int bit = (data & 8) >> 3;

	switch (idx)
	{
		case 0: set_interrupt(cpunum, IRQ_IPI2_LEVEL, bit ? ASSERT_LINE : CLEAR_LINE);
				break;
		case 2: // TODO: Hardware trace
				break;
		case 4: m_cpu_map_switch[cpunum] = bit;
				break;
		case 6: if (cpunum == CPU_1)
					m_maincpu1->set_input_line(M6809_FIRQ_LINE, bit ? ASSERT_LINE : CLEAR_LINE);
				else
					m_maincpu2->set_input_line(M6809_FIRQ_LINE, bit ? ASSERT_LINE : CLEAR_LINE);
				break;
	}
}

uint8_t cmi_state::parity_r(offs_t offset)
{
	m_maincpu2_irq0_merger->in_w<1>(0);
	LOG("%s: parity_r %04x\n", machine().describe_context(), offset);
	return 0xff;
}

void cmi_state::mapsel_w(offs_t offset, uint8_t data)
{
	LOG("%s: mapsel_w: %02x = %02x\n", machine().describe_context(), offset, data);

	data ^= 0x1f;
	m_map_sel[offset] = data;

	if ((offset == MAPSEL_P1_A) && (m_cpu_active_space[0] == MAPPING_A))
		update_address_space(0, data);
	else if ((offset == MAPSEL_P1_B) && (m_cpu_active_space[0] == MAPPING_B))
		update_address_space(0, data);

	if ((offset == MAPSEL_P2_A) && (m_cpu_active_space[1] == MAPPING_A))
		update_address_space(1, data);
	else if ((offset == MAPSEL_P2_B) && (m_cpu_active_space[1] == MAPPING_B))
		update_address_space(1, data);
}



void cmi_state::midi_dma_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	address_space *cmi_space = ((offset & 0x8000) ? m_cpu2space : m_cpu1space);
	offset &= 0x7fff;

	if (ACCESSING_BITS_0_7)
		cmi_space->write_byte(offset * 2 + 1, data);
	if (ACCESSING_BITS_8_15)
		cmi_space->write_byte(offset * 2, data >> 8);
}

uint16_t cmi_state::midi_dma_r(offs_t offset)
{
	address_space *cmi_space = ((offset & 0x8000) ? m_cpu2space : m_cpu1space);
	offset &= 0x7fff;
	return cmi_space->read_word(offset * 2);
}

WRITE_LINE_MEMBER( cmi_state::midi_ptm0_c3_w )
{
	m_midi_ptm[1]->set_clock(0, state);
	m_midi_ptm[1]->set_clock(1, state);
	m_midi_ptm[1]->set_clock(2, state);
}

WRITE_LINE_MEMBER( cmi_state::midi_ptm_irq )
{
	m_midicpu->set_input_line(M68K_IRQ_2, state ? ASSERT_LINE : CLEAR_LINE);
}

WRITE_LINE_MEMBER( cmi_state::midi_acia_irq )
{
	m_midicpu->set_input_line(M68K_IRQ_3, state ? ASSERT_LINE : CLEAR_LINE);
}

void cmi_state::midi_latch_w(uint8_t data)
{
	const uint8_t bit_offset = data & 0x7;
	const uint8_t bit_value = BIT(data, 3);
	switch (bit_offset)
	{
	case 0x00: // /INT1
		LOG("%s: %sing INT1 line on SMIDI card\n", machine().describe_context(), bit_value ? "Clear" : "Sett");
		m_midicpu->set_input_line(M68K_IRQ_1, bit_value ? CLEAR_LINE : ASSERT_LINE);
		break;
	case 0x02: // SMIDINT L0
		LOG("%s: %sing SMIDI to CMI interrupt P1 level 0\n", machine().describe_context(), bit_value ? "Sett" : "Clear");
		set_interrupt(CPU_1, IRQ_MIDINT_LEVEL, bit_value ? ASSERT_LINE : CLEAR_LINE);
		break;
	case 0x03: // /INT7
		LOG("%s: %sing INT7 line on SMIDI card\n", machine().describe_context(), bit_value ? "Clear" : "Sett");
		m_midicpu->set_input_line(M68K_IRQ_7, bit_value ? CLEAR_LINE : ASSERT_LINE);
		break;
	case 0x04: // SMIDINT L3
		LOG("%s: %sing SMIDI to CMI interrupt P1 level 3\n", machine().describe_context(), bit_value ? "Sett" : "Clear");
		set_interrupt(CPU_1, IRQ_SMIDINT_LEVEL, bit_value ? ASSERT_LINE : CLEAR_LINE);
		break;
	case 0x05: // /HALT
		LOG("%s: %sing HALT line on SMIDI card\n", machine().describe_context(), bit_value ? "Clear" : "Sett");
		m_midicpu->set_input_line(INPUT_LINE_HALT, bit_value ? CLEAR_LINE : ASSERT_LINE);
		break;
	case 0x06: // SYNCSW
		LOG("%s: %sing SYNCSW line on SMIDI card\n", machine().describe_context(), bit_value ? "Sett" : "Clear");
		break;
	case 0x07: // /RESET
		LOG("%s: %sing RESET line on SMIDI card\n", machine().describe_context(), bit_value ? "Clear" : "Sett");
		m_midicpu->set_input_line(INPUT_LINE_RESET, bit_value ? CLEAR_LINE : ASSERT_LINE);
		break;
	}
}

void cmi_state::maincpu1_map(address_map &map)
{
	map(0x0000, 0x7fff).rw(&cmi_state::ram_range_r<0, 0x0000>, "cmi_state::ram_range_r<0, 0x0000>",
		&cmi_state::ram_range_w<0, 0x0000>, "cmi_state::ram_range_w<0, 0x0000>");
	map(0x8000, 0xbfff).rw(FUNC(cmi_state::vram_range_r<0>), FUNC(cmi_state::vram_range_w<0>));
	map(0xc000, 0xdfff).rw(&cmi_state::ram_range_r<0, 0xc000>, "cmi_state::ram_range_r<0, 0xc000>",
		&cmi_state::ram_range_w<0, 0xc000>, "cmi_state::ram_range_w<0, 0xc000>");
	map(0xe000, 0xefff).rw(FUNC(cmi_state::cards_range_r<0>), FUNC(cmi_state::cards_range_w<0>));
	map(0xf000, 0xffff).rw(FUNC(cmi_state::periphs_range_r<0>), FUNC(cmi_state::periphs_range_w<0>));
}

void cmi_state::maincpu2_map(address_map &map)
{
	map(0x0000, 0x7fff).rw(&cmi_state::ram_range_r<1, 0x0000>, "cmi_state::ram_range_r<1, 0x0000>",
		&cmi_state::ram_range_w<1, 0x0000>, "cmi_state::ram_range_w<1, 0x0000>");
	map(0x8000, 0xbfff).rw(FUNC(cmi_state::vram_range_r<1>), FUNC(cmi_state::vram_range_w<1>));
	map(0xc000, 0xdfff).rw(&cmi_state::ram_range_r<1, 0xc000>, "cmi_state::ram_range_r<1, 0xc000>",
		&cmi_state::ram_range_w<1, 0xc000>, "cmi_state::ram_range_w<1, 0xc000>");
	map(0xe000, 0xefff).rw(FUNC(cmi_state::cards_range_r<1>), FUNC(cmi_state::cards_range_w<1>));
	map(0xf000, 0xffff).rw(FUNC(cmi_state::periphs_range_r<1>), FUNC(cmi_state::periphs_range_w<1>));
}

void cmi_state::midicpu_map(address_map &map)
{
	map(0x000000, 0x003fff).rom();
	map(0x040000, 0x05ffff).rw(FUNC(cmi_state::midi_dma_r), FUNC(cmi_state::midi_dma_w));
	map(0x060000, 0x06000f).rw(m_midi_ptm[0], FUNC(ptm6840_device::read), FUNC(ptm6840_device::write)).umask16(0xff00);
	map(0x060010, 0x06001f).rw(m_midi_ptm[1], FUNC(ptm6840_device::read), FUNC(ptm6840_device::write)).umask16(0xff00);
	map(0x060020, 0x06005f).rw(m_midi_acia[0], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0x00ff);
	map(0x060030, 0x06003f).rw(m_midi_acia[1], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0x00ff);
	map(0x060040, 0x06004f).rw(m_midi_acia[2], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0x00ff);
	map(0x060050, 0x06005f).rw(m_midi_acia[3], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0x00ff);
	//map(0x060060, 0x06007f) SMPTE
	map(0x080000, 0x083fff).ram();
}

void cmi_state::cmi07cpu_map(address_map &map)
{
	map(0x0000, 0x0000).r(FUNC(cmi_state::aic_ad574_r)).mirror(0x3fff);
	map(0x4000, 0x4000).w(FUNC(cmi_state::aic_dac_w<0>)).mirror(0x3ff8);
	map(0x4001, 0x4001).w(FUNC(cmi_state::aic_dac_w<1>)).mirror(0x3ff8);
	map(0x4002, 0x4002).w(FUNC(cmi_state::aic_dac_w<2>)).mirror(0x3ff8);
	map(0x4003, 0x4003).w(FUNC(cmi_state::aic_dac_w<3>)).mirror(0x3ff8);
	map(0x4004, 0x4004).w(FUNC(cmi_state::aic_mux_latch_w)).mirror(0x3ff8);
	map(0x4006, 0x4006).w(FUNC(cmi_state::aic_ad565_msb_w)).mirror(0x3ff8);
	map(0x4007, 0x4007).w(FUNC(cmi_state::aic_ad565_lsb_w)).mirror(0x3ff8);
	map(0x8000, 0x8fff).rw(m_cmi07_ptm, FUNC(ptm6840_device::read), FUNC(ptm6840_device::write));
	map(0xc000, 0xffff).ram().share("cmi07_ram");
}

void cmi_state::cpu1_periphs_map(address_map &map)
{
	map(0x0000, 0x07ff).rw(FUNC(cmi_state::rom_r<0>), FUNC(cmi_state::map_ram_w));
	map(0x0800, 0x0bff).rom().region("q133", 0x2800);
	map(0x0c40, 0x0c4f).rw(FUNC(cmi_state::parity_r), FUNC(cmi_state::mapsel_w));
	map(0x0c5a, 0x0c5b).noprw(); // Q077 HDD controller - not installed
	map(0x0c5e, 0x0c5e).rw(FUNC(cmi_state::atomic_r), FUNC(cmi_state::cpufunc_w));
	map(0x0c5f, 0x0c5f).rw(FUNC(cmi_state::map_r<0>), FUNC(cmi_state::map_w<0>));
	map(0x0c80, 0x0c83).rw(m_q133_acia[0], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c84, 0x0c87).rw(m_q133_acia[1], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c88, 0x0c8b).rw(m_q133_acia[2], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c8c, 0x0c8f).rw(m_q133_acia[3], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c90, 0x0c97).rw(m_q133_ptm, FUNC(ptm6840_device::read), FUNC(ptm6840_device::write));
	//map(0x0ca0, 0x0ca0).w(FUNC(cmi_state::midi_latch_w));
	map(0x0cbc, 0x0cbc).rw(FUNC(cmi_state::cmi07_r), FUNC(cmi_state::cmi07_w));
	map(0x0cc0, 0x0cc3).r(FUNC(cmi_state::lightpen_r));
	map(0x0cc4, 0x0cc7).rw(m_q219_pia, FUNC(pia6821_device::read), FUNC(pia6821_device::write));
	map(0x0cc8, 0x0ccf).rw(m_q219_ptm, FUNC(ptm6840_device::read), FUNC(ptm6840_device::write));
	map(0x0cd0, 0x0cdc).rw(FUNC(cmi_state::video_r), FUNC(cmi_state::video_w));
	map(0x0ce0, 0x0ce1).rw(FUNC(cmi_state::fdc_r), FUNC(cmi_state::fdc_w));
	map(0x0ce2, 0x0cef).noprw(); // Monitor ROM will attempt to detect floppy disk controller cards in this entire range
	map(0x0cf0, 0x0cf7).rw(m_q133_pia[0], FUNC(pia6821_device::read), FUNC(pia6821_device::write));
	map(0x0cf8, 0x0cff).rw(m_q133_pia[1], FUNC(pia6821_device::read), FUNC(pia6821_device::write));
	map(0x0cfc, 0x0cfc).w(FUNC(cmi_state::i8214_cpu1_w));
	map(0x0cfd, 0x0cfd).w(FUNC(cmi_state::i8214_cpu2_w));
	map(0x0d00, 0x0eff).rw(FUNC(cmi_state::shared_ram_r), FUNC(cmi_state::shared_ram_w));
	map(0x0f00, 0x0ff7).rw(FUNC(cmi_state::scratch_ram_r<0>), FUNC(cmi_state::scratch_ram_w<0>));
	map(0x0ff8, 0x0ff9).rw(FUNC(cmi_state::irq_ram_r<0>), FUNC(cmi_state::irq_ram_w<0>));
	map(0x0ffa, 0x0ffd).rw(FUNC(cmi_state::scratch_ram_fa_r<0>), FUNC(cmi_state::scratch_ram_fa_w<0>));
	map(0x0ffe, 0x0fff).r(FUNC(cmi_state::vector_r<0>));
}

void cmi_state::cpu2_periphs_map(address_map &map)
{
	map(0x0000, 0x07ff).rw(FUNC(cmi_state::rom_r<1>), FUNC(cmi_state::map_ram_w));
	map(0x0800, 0x0bff).rom().region("q133", 0x1800);
	map(0x0c40, 0x0c4f).rw(FUNC(cmi_state::parity_r), FUNC(cmi_state::mapsel_w));
	map(0x0c5a, 0x0c5b).noprw(); // Q077 HDD controller - not installed
	map(0x0c5e, 0x0c5e).rw(FUNC(cmi_state::atomic_r), FUNC(cmi_state::cpufunc_w));
	map(0x0c5f, 0x0c5f).rw(FUNC(cmi_state::map_r<1>), FUNC(cmi_state::map_w<1>));
	map(0x0c80, 0x0c83).rw(m_q133_acia[0], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c84, 0x0c87).rw(m_q133_acia[1], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c88, 0x0c8b).rw(m_q133_acia[2], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c8c, 0x0c8f).rw(m_q133_acia[3], FUNC(mos6551_device::read), FUNC(mos6551_device::write));
	map(0x0c90, 0x0c97).rw(m_q133_ptm, FUNC(ptm6840_device::read), FUNC(ptm6840_device::write));
	//map(0x0ca0, 0x0ca0).w(FUNC(cmi_state::midi_latch_w));
	map(0x0cc0, 0x0cc3).r(FUNC(cmi_state::lightpen_r));
	map(0x0cc4, 0x0cc7).rw(m_q219_pia, FUNC(pia6821_device::read), FUNC(pia6821_device::write));
	map(0x0cc8, 0x0ccf).rw(m_q219_ptm, FUNC(ptm6840_device::read), FUNC(ptm6840_device::write));
	map(0x0cd0, 0x0cdc).rw(FUNC(cmi_state::video_r), FUNC(cmi_state::video_w));
	map(0x0ce0, 0x0ce1).rw(FUNC(cmi_state::fdc_r), FUNC(cmi_state::fdc_w));
	map(0x0ce2, 0x0cef).noprw(); // Monitor ROM will attempt to detect floppy disk controller cards in this entire range
	map(0x0cf0, 0x0cf7).rw(m_q133_pia[0], FUNC(pia6821_device::read), FUNC(pia6821_device::write));
	map(0x0cf8, 0x0cff).rw(m_q133_pia[1], FUNC(pia6821_device::read), FUNC(pia6821_device::write));
	map(0x0cfc, 0x0cfc).w(FUNC(cmi_state::i8214_cpu1_w));
	map(0x0cfd, 0x0cfd).w(FUNC(cmi_state::i8214_cpu2_w));
	map(0x0d00, 0x0eff).rw(FUNC(cmi_state::shared_ram_r), FUNC(cmi_state::shared_ram_w));
	map(0x0f00, 0x0ff7).rw(FUNC(cmi_state::scratch_ram_r<1>), FUNC(cmi_state::scratch_ram_w<1>));
	map(0x0ff8, 0x0ff9).rw(FUNC(cmi_state::irq_ram_r<1>), FUNC(cmi_state::irq_ram_w<1>));
	map(0x0ffa, 0x0ffd).rw(FUNC(cmi_state::scratch_ram_fa_r<1>), FUNC(cmi_state::scratch_ram_fa_w<1>));
	map(0x0ffe, 0x0fff).r(FUNC(cmi_state::vector_r<1>));
}

/* Input ports */
static INPUT_PORTS_START( cmi2x )
	PORT_START("LP_X")
	PORT_BIT( 0xffff, HBLANK_START/2, IPT_LIGHTGUN_X) PORT_NAME ("Lightpen X") PORT_MINMAX(0, HBLANK_START - 1) PORT_SENSITIVITY(50) PORT_CROSSHAIR(X, 1.0, 0.0, 0)

	PORT_START("LP_Y")
	PORT_BIT( 0xffff, VBLANK_START/2, IPT_LIGHTGUN_Y) PORT_NAME ("Lightpen Y") PORT_MINMAX(0, VBLANK_START - 1) PORT_SENSITIVITY(50) PORT_CROSSHAIR(Y, 1.0, 0.0, 0)

	PORT_START("LP_TOUCH")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_NAME ( "Lightpen Touch" ) PORT_CODE( MOUSECODE_BUTTON1 )
INPUT_PORTS_END

template <int cpunum> uint8_t cmi_state::scratch_ram_r(offs_t offset)
{
	return m_scratch_ram[cpunum][offset];
}

template <int cpunum> void cmi_state::scratch_ram_w(offs_t offset, uint8_t data)
{
	m_scratch_ram[cpunum][offset] = data;
}

template <int cpunum> uint8_t cmi_state::scratch_ram_fa_r(offs_t offset)
{
	return m_scratch_ram[cpunum][0xfa + offset];
}

template <int cpunum> void cmi_state::scratch_ram_fa_w(offs_t offset, uint8_t data)
{
	m_scratch_ram[cpunum][0xfa + offset] = data;
}

bool cmi_state::map_is_active(int cpunum, int map, uint8_t *map_info)
{
	if (m_cpu_active_space[cpunum] == MAPPING_A)
	{
		*map_info = m_map_sel[cpunum ? MAPSEL_P2_A : MAPSEL_P1_A];

		if ((*map_info & 0x1f) == map)
			return true;
	}
	else
	{
		*map_info = m_map_sel[cpunum ? MAPSEL_P2_B : MAPSEL_P1_B];

		if ((*map_info & 0x1f) == map)
			return true;
	}

	return false;
}

void cmi_state::update_address_space(int cpunum, uint8_t mapinfo)
{
	m_curr_mapinfo[cpunum] = mapinfo;
}

void cmi_state::cmi07_w(uint8_t data)
{
	LOG("%s: cmi07_w: %02x\n", machine().describe_context(), data);

	if (true)
	{
		return;
	}

	const uint8_t prev = m_cmi07_ctrl;
	m_cmi07_ctrl = data;

	m_cmi07_base_enable[0] = BIT(data, 6) && (data & 0x30);
	m_cmi07_base_enable[1] = m_cmi07_base_enable[0] && !BIT(data, 7);
	m_cmi07_base_addr = (data & 0x30) << 10;

	m_cmi07cpu->set_input_line(INPUT_LINE_RESET, BIT(data, 0) ? CLEAR_LINE : ASSERT_LINE);
	m_cmi07cpu->set_input_line(M6809_FIRQ_LINE,  BIT(data, 1) ? CLEAR_LINE : ASSERT_LINE);
	m_cmi07cpu->set_input_line(INPUT_LINE_NMI,   BIT(data, 2) ? CLEAR_LINE : ASSERT_LINE);
	m_cmi07cpu->set_input_line(INPUT_LINE_HALT,  BIT(data, 3) ? CLEAR_LINE : ASSERT_LINE);

	/* We need to update the address spaces */
	uint8_t map_info = (m_cpu_active_space[0] == MAPPING_A) ? m_map_sel[MAPSEL_P1_A] : m_map_sel[MAPSEL_P1_B];
	update_address_space(0, map_info);

	/* CPU 2 space is untouched by this update */
	if (BIT(prev & data, 7))
		return;

	map_info = (m_cpu_active_space[1] == MAPPING_A) ? m_map_sel[MAPSEL_P2_A] : m_map_sel[MAPSEL_P2_B];
	update_address_space(1, map_info);
}

uint8_t cmi_state::cmi07_r()
{
	LOG("%s: cmi07_r: %02x\n", machine().describe_context(), 0xff);
	return 0xff;
}

WRITE_LINE_MEMBER( cmi_state::q133_ptm_irq_w )
{
	m_maincpu2_irq0_merger->in_w<0>(state);
}

WRITE_LINE_MEMBER( cmi_state::q133_acia_irq )
{
	set_interrupt(CPU_1, IRQ_ACINT_LEVEL, state ? ASSERT_LINE : CLEAR_LINE);
}


/**************************************
 *
 *  Floppy disk interface
 *
 *************************************/

void cmi_state::dma_fdc_rom()
{
	/* DMA channel 1 is used*/
	uint8_t map_info = m_map_sel[MAPSEL_P2_A_DMA1];
	int map = map_info & 0x1f;
	int addr = m_fdc_dma_addr.w.l & ~PAGE_MASK;
	int page = addr / PAGE_SIZE;
	uint8_t p_info = 0;

	/* Active low */
	m_fdc_status &= ~FDC_STATUS_DRIVER_LOAD;

	int i = 0;
	for (i = 0; i < NUM_Q256_CARDS; ++i)
	{
		p_info = m_map_ram[i][(map << PAGE_SHIFT) | page];

		if (p_info & 0x80)
			break;
	}

	if ((p_info & 0x80) == 0)
	{
		osd_printf_debug("Trying to DMA FDC driver to a non-enabled page!\n");
		return;
	}

	/* TODO: This should be stuck in a deferred write */
	int cnt = std::min(m_fdc_dma_cnt.w.l ^ 0xffff, 2048);
	memcpy(&m_q256_ram[i][(p_info & 0x7f) * PAGE_SIZE], m_qfc9_region_ptr, cnt);
	m_fdc_status |= FDC_STATUS_DRIVER_LOAD;

	/* TODO: Is this correct? */
	m_fdc_dma_addr.w.l += 0x800;
	m_fdc_dma_cnt.w.l = 0;
}

void cmi_state::write_fdc_ctrl(uint8_t data)
{
	int drive = data & 1;
	int side = BIT(data, 5) ? 1 : 0;

	switch (drive)
	{
	case 0: m_wd1791->set_floppy(m_floppy0->get_device()); break;
	case 1: m_wd1791->set_floppy(m_floppy1->get_device()); break;
	}

	if (m_floppy0->get_device()) m_floppy0->get_device()->ss_w(side);
	if (m_floppy1->get_device()) m_floppy1->get_device()->ss_w(side);

	m_wd1791->dden_w(BIT(data, 7) ? true : false);

	m_fdc_ctrl = data;
}

void cmi_state::fdc_w(offs_t offset, uint8_t data)
{
	if (offset == 0)
	{
		switch (m_fdc_addr)
		{
			case 0x0: write_fdc_ctrl(data);         break;
			case 0x2: m_fdc_dma_addr.b.l = data;    break;
			case 0x4: m_fdc_dma_addr.b.h  = data;   break;
			case 0x6: m_fdc_dma_cnt.b.l = data;     break;
			case 0x8: m_fdc_dma_cnt.b.h = data;     break;
			case 0xa: dma_fdc_rom();                break;
			case 0xc: m_wd1791->cmd_w(data ^ 0xff);        break;
			case 0xd: m_wd1791->track_w(data ^ 0xff);      break;
			case 0xe: m_wd1791->sector_w(data ^ 0xff);     break;
			case 0xf: m_wd1791->data_w(data ^ 0xff);       break;
			default: osd_printf_debug("fdc_w: Invalid access (%x with %x)", m_fdc_addr, data);
		}
	}
	else
		m_fdc_addr = data;
}

uint8_t cmi_state::fdc_r(offs_t offset)
{
	if (machine().side_effects_disabled())
		return 0;

	if (offset == 0)
	{
		switch (m_fdc_addr)
		{
			case 0xc: return m_wd1791->status_r() ^ 0xff;
			case 0xd: return m_wd1791->track_r() ^ 0xff;
			case 0xe: return m_wd1791->sector_r() ^ 0xff;
			case 0xf: return m_wd1791->data_r() ^ 0xff;
			default:  return 0;
		}
	}
	else
		return m_fdc_status;
}

void cmi_state::fdc_dma_transfer()
{
	/* DMA channel 1 is used*/
	uint8_t map_info = m_map_sel[MAPSEL_P2_A_DMA1];
	int map = map_info & 0x1f;

	int cpu_page = (m_fdc_dma_addr.w.l & ~PAGE_MASK) / PAGE_SIZE;
	int phys_page = 0;

	int i = 0;
	for (i = 0; i < NUM_Q256_CARDS; ++i)
	{
		phys_page = m_map_ram[i][(map << PAGE_SHIFT) | cpu_page];

		if (phys_page & 0x80)
			break;
	}

	//phys_page &= 0x7f;

	/* Transfer from disk to RAM */
	if (!BIT(m_fdc_ctrl, 4))
	{
		/* Read a byte at a time */
		uint8_t data = m_wd1791->data_r() ^ 0xff;

		if (m_fdc_dma_cnt.w.l == 0xffff)
			return;

		if (m_cmi07_ctrl & 0x30)
			if (BIT(m_cmi07_ctrl, 6) && !BIT(m_cmi07_ctrl, 7))
			{
				if ((m_fdc_dma_addr.w.l & 0xc000) == ((m_cmi07_ctrl & 0x30) << 10))
					m_cmi07_ram[m_fdc_dma_addr.w.l & 0x3fff] = data;
			}

		if (phys_page & 0x80)
			m_q256_ram[i][((phys_page & 0x7f) * PAGE_SIZE) + (m_fdc_dma_addr.w.l & PAGE_MASK)] = data;

		if (!BIT(m_fdc_ctrl, 3))
			m_fdc_dma_addr.w.l++;
	}

	// Transfer from RAM to disk
	else
	{
		if (m_fdc_dma_cnt.w.l == 0xffff)
			return;

		/* Write a byte at a time */
		uint8_t data = 0;

		/* TODO: This should be stuck in a deferred write */
		if (phys_page & 0x80)
			data = m_q256_ram[i][((phys_page & 0x7f) * PAGE_SIZE) + (m_fdc_dma_addr.w.l & PAGE_MASK)];

		m_wd1791->data_w(data ^ 0xff);

		if (!BIT(m_fdc_ctrl, 3))
			m_fdc_dma_addr.w.l++;
	}

	m_fdc_dma_cnt.w.l++;
}

WRITE_LINE_MEMBER( cmi_state::wd1791_irq )
{
	if (state)
	{
		m_fdc_status |= FDC_STATUS_INTERRUPT;

		if (m_fdc_ctrl & FDC_CONTROL_INTEN)
			set_interrupt(CPU_2, IRQ_DISKINT_LEVEL, ASSERT_LINE);

	}
	else
	{
		m_fdc_status &= ~FDC_STATUS_INTERRUPT;
		set_interrupt(CPU_2, IRQ_DISKINT_LEVEL, CLEAR_LINE);
	}
}

WRITE_LINE_MEMBER( cmi_state::wd1791_drq )
{
	m_fdc_drq = state;
	if (state)
		fdc_dma_transfer();
}



/**************************************
 *
 *  Master card and channel cards
 *
 *************************************/

/*
0 - 1f
20 = PIA
21 = PIA
22 = PIA
23 = PIA

24 = ADC
25 = ADC
26 = HALT CPU 2
27 = UNHALT CPU 2
28 - 2B = PIA
*/

void cmi_state::master_tune_w(uint8_t data)
{
//  double mfreq = (double)data * ((double)MASTER_OSCILLATOR / 2.0) / 256.0;
}

void cmi_state::cmi02_chsel_w(uint8_t data)
{
	m_cmi02_pia_chsel = data;
}

WRITE_LINE_MEMBER( cmi_state::cmi02_ptm_irq )
{
	LOG("%s: cmi02_ptm_irq: %d\n", machine().describe_context(), state);
	m_cmi02_ptm_irq = state;
	set_interrupt(CPU_1, IRQ_TIMINT_LEVEL, m_cmi02_ptm_irq ? ASSERT_LINE : CLEAR_LINE);
}

WRITE_LINE_MEMBER( cmi_state::cmi02_ptm_o2 )
{
	m_cmi02_ptm->set_c1(state);
	m_cmi02_ptm->set_c3(state);
}

WRITE_LINE_MEMBER( cmi_state::cmi02_pia2_irqa_w )
{
	LOG("%s: cmi02_pia2_irqa_w: %d\n", machine().describe_context(), state);
	set_interrupt(CPU_2, IRQ_ADINT_LEVEL, state ? ASSERT_LINE : CLEAR_LINE);
}

WRITE_LINE_MEMBER( cmi_state::cmi02_pia2_cb2_w )
{
	LOG("%s: cmi02_pia2_cb2_w: %d\n", machine().describe_context(), state);
	m_cmi02_pia[1]->ca1_w(1);
	m_cmi02_pia[1]->ca1_w(0);
}

uint8_t cmi_state::cmi02_r(offs_t offset)
{
	if (machine().side_effects_disabled())
		return 0;

	if (offset <= 0x1f)
	{
		int ch_mask = m_cmi02_pia_chsel;

		for (int i = 0; i < 8; ++i)
		{
			if (ch_mask & (1 << i))
			{
				return m_channels[i]->read(offset & 0x1f);
			}
		}

		return 0xff;
	}
	else
	{
		uint8_t data = 0;
		switch (offset)
		{
			case 0x20: case 0x21: case 0x22: case 0x23:
				data = m_cmi02_pia[0]->read(offset & 3);
				LOG("%s: CMI02 PIA 1 read (offset %d): %02x\n", machine().describe_context(), offset & 3, data);
				return data;

			case 0x26:
				m_maincpu2->set_input_line(INPUT_LINE_HALT, ASSERT_LINE);
				/* LS123 one-shot with 10n and 150k */
				m_jam_timeout_timer->adjust(attotime::from_usec(675));
				LOG("%s: CMI02 Jam Timeout timer trigger read: %02x\n", machine().describe_context(), 0xff);
				return 0xff;

			case 0x27:
				m_maincpu2->set_input_line(INPUT_LINE_HALT, CLEAR_LINE);
				LOG("%s: CMI02 CPU2 Unhalt read: %02x\n", machine().describe_context(), 0xff);
				return 0xff;

			case 0x28: case 0x29: case 0x2a: case 0x2b:
				data = m_cmi02_pia[1]->read(offset & 3);
				LOG("%s: CMI02 PIA 2 read (offset %d): %02x\n", machine().describe_context(), offset & 3, data);
				return data;

			case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
				data = m_cmi02_ptm->read(offset & 7);
				LOG("%s: CMI02 PTM read (offset %d): %02x\n", machine().describe_context(), offset & 7, data);
				return data;

			default:
				LOG("%s: CMI02 Unknown read (offset %02x): %02x\n", machine().describe_context(), offset, 0x00);
				return data;
		}
	}
}

void cmi_state::cmi02_w(offs_t offset, uint8_t data)
{
	if (offset <= 0x1f)
	{
		int ch_mask = m_cmi02_pia_chsel;

		for (int i = 0; i < 8; ++i)
		{
			if (ch_mask & (1 << i))
				m_channels[i]->write(offset & 0x1f, data);
		}
	}
	else
	{
		switch (offset)
		{
			case 0x20: case 0x21: case 0x22: case 0x23:
				LOG("%s: CMI02 PIA 1 write (offset %d): %02x\n", machine().describe_context(), offset & 3, data);
				m_cmi02_pia[0]->write(offset & 3, data);
				break;

			case 0x28: case 0x29: case 0x2a: case 0x2b:
				LOG("%s: CMI02 PIA 2 write (offset %d): %02x\n", machine().describe_context(), offset & 3, data);
				m_cmi02_pia[1]->write(offset & 3, data);
				break;

			case 0x30:
				LOG("%s: CMI02 PICU 3 B/SGS write (clearing main CPU IRQ merger, clearing high-prio IRQ): %02x\n", machine().describe_context(), data);
				m_hp_int = 0;
				m_maincpu1_irq_merger->in_w<1>(0);
				//if (m_lp_int == 0)
				//  m_maincpu1->set_input_line(M6809_IRQ_LINE, CLEAR_LINE);
				m_i8214[2]->b_sgs_w(~(data & 0xf));
				break;

			case 0x31: case 0x32:
				LOG("%s: CMI02 INTP1 %s write: %02x\n", machine().describe_context(), (offset & 2) ? "clear" : "set", data);
				set_interrupt(0, IRQ_INTP1_LEVEL, (offset & 2) ? CLEAR_LINE : ASSERT_LINE);
				break;

			case 0x33: case 0x34:
				LOG("%s: CMI02 INTP2 %s write: %02x\n", machine().describe_context(), (offset & 4) ? "clear" : "set", data);
				set_interrupt(1, IRQ_INTP2_LEVEL, (offset & 4) ? CLEAR_LINE : ASSERT_LINE);
				break;

			case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
				LOG("%s: CMI02 PTM write (offset %d): %02x\n", machine().describe_context(), offset & 7, data);
				m_cmi02_ptm->write(offset & 7, data);
				break;

			default:
				LOG("%s: CMI02 Unknown write (offset %02x): %02x\n", machine().describe_context(), offset, data);
				break;
		}
	}
}

template<int Channel>
WRITE_LINE_MEMBER(cmi_state::channel_irq)
{
	set_interrupt(CPU_1, ch_int_levels[Channel], state);
}

void cmi_state::i8214_cpu1_w(uint8_t data)
{
	//LOG("%s: i8214_cpu1_w, clearing IRQ merger bit 0: %02x\n", machine().describe_context(), data);
	m_maincpu1_irq_merger->in_w<0>(0);
	m_lp_int = 0;
	//if (m_hp_int == 0)
	//  m_maincpu1->set_input_line(M6809_IRQ_LINE, CLEAR_LINE);
	m_i8214[0]->b_sgs_w(~(data & 0xf));
}


void cmi_state::i8214_cpu2_w(uint8_t data)
{
	LOG("%s: i8214_cpu2_w, clearing CPU2 IRQ line: %02x\n", machine().describe_context(), data);
	m_maincpu2->set_input_line(M6809_IRQ_LINE, CLEAR_LINE);
	m_i8214[1]->b_sgs_w(~(data & 0xf));
}

uint8_t cmi_state::shared_ram_r(offs_t offset)
{
	return m_shared_ram[offset];
}

void cmi_state::shared_ram_w(offs_t offset, uint8_t data)
{
	m_shared_ram[offset] = data;
}

uint8_t cmi_state::aic_ad574_r()
{
	const bool adca = BIT(m_aic_mux_latch, 5); // false - MSB, true - LSB
	const uint16_t val = m_aic_ad565_in[m_aic_mux_latch & 0x07];
	const uint8_t data = adca ? ((uint8_t)val) : (val >> 8);
	LOG("%s: AIC AD574 read: %02x\n", machine().describe_context(), data);
	return data;
}

template<int Dac> void cmi_state::aic_dac_w(uint8_t data)
{
	LOG("%s: AIC DAC%d write: %02x\n", machine().describe_context(), Dac + 1, data);
	// To Do
}

void cmi_state::aic_mux_latch_w(uint8_t data)
{
	LOG("%s: AIC mux latch write: %02x\n", machine().describe_context(), data);
	set_interrupt(CPU_1, IRQ_AIC_LEVEL, BIT(data, 7) ? ASSERT_LINE : CLEAR_LINE);
	if (data & 0x07)
	{
		m_aic_mux_latch = data;
	}
	else
	{
		m_aic_mux_latch &= 0x07;
		m_aic_mux_latch |= data & 0xf8;
	}
	if (!BIT(data, 6))
	{
		LOG("%s: ADCR is 0, initiating ADC conversion request\n", machine().describe_context());
		m_aic_ad565_in[m_aic_mux_latch & 0x07] = 0;
	}
}

void cmi_state::aic_ad565_msb_w(uint8_t data)
{
	m_aic_ad565_in[m_aic_mux_latch & 0x07] &= 0x00ff;
	m_aic_ad565_in[m_aic_mux_latch & 0x07] |= (uint16_t)data << 8;
	LOG("%s: AIC AD565 MSB write: %02x, input %04x\n", machine().describe_context(), data, m_aic_ad565_in[m_aic_mux_latch & 0x07]);
}

void cmi_state::aic_ad565_lsb_w(uint8_t data)
{
	LOG("%s: AIC AD565 LSB write: %02x\n", machine().describe_context(), data);
	m_aic_ad565_in[m_aic_mux_latch & 0x07] &= 0xff00;
	m_aic_ad565_in[m_aic_mux_latch & 0x07] |= data;
}

/*************************************
 *
 *  Interrupt Handling
 *
 *************************************/

WRITE_LINE_MEMBER( cmi_state::ptm_q219_irq )
{
	set_interrupt(CPU_2, IRQ_RINT_LEVEL, state);
}

IRQ_CALLBACK_MEMBER( cmi_state::cpu1_interrupt_callback )
{
	/* Switch to mapping A */
	m_cpu_active_space[CPU_1] = MAPPING_A;
	update_address_space(CPU_1, m_map_sel[MAPSEL_P1_A]);

	if (irqline == INPUT_LINE_IRQ0)
	{
		int vector = (m_hp_int ? 0xffe0 : 0xffd0);
		int level = (m_hp_int ? m_i8214[2]->a_r() : m_i8214[0]->a_r()) ^ 7;
		m_irq_address[CPU_1][0] = m_cpu1space->read_byte(vector + level*2);
		m_irq_address[CPU_1][1] = m_cpu1space->read_byte(vector + level*2 + 1);

		m_m6809_bs_hack_cnt[CPU_1] = 2;

		LOG("%s: CPU1 interrupt, will be pushing address %02x%02x\n", machine().describe_context(), m_irq_address[CPU_1][0], m_irq_address[CPU_1][1]);
	}
	else
	{
		LOG("%s: Some other CPU1 interrupt, line %d\n", irqline);
	}

	return 0;
}

IRQ_CALLBACK_MEMBER( cmi_state::cpu2_interrupt_callback )
{
	/* Switch to mapping A */
	m_cpu_active_space[CPU_2] = MAPPING_A;
	update_address_space(CPU_2, m_map_sel[MAPSEL_P2_A]);

	if (irqline == INPUT_LINE_IRQ0)
	{
		int level = m_i8214[1]->a_r() ^ 7;
		m_irq_address[CPU_2][0] = m_cpu2space->read_byte(0xffe0 + level*2);
		m_irq_address[CPU_2][1] = m_cpu2space->read_byte(0xffe0 + level*2 + 1);

		m_m6809_bs_hack_cnt[CPU_2] = 2;

		//osd_printf_debug("cpu1 interrupt, will be pushing address %02x%02x\n", m_irq_address[CPU_2][0], m_irq_address[CPU_2][1]);
	}
	return 0;
}

void cmi_state::set_interrupt(int cpunum, int level, int state)
{
	LOG("%s: CPU%d Int: %x State: %x\n", machine().describe_context(), cpunum + 1, level, state);

	if (state == ASSERT_LINE)
		m_int_state[cpunum] |= (1 << level);
	else
		m_int_state[cpunum] &= ~(1 << level);

	if (cpunum == 0)
	{
		if (level < 8)
			m_i8214[2]->r_all_w(~(m_int_state[cpunum]));
		else
			m_i8214[0]->r_all_w(~(m_int_state[cpunum] >> 8));
	}
	else
	{
		m_i8214[1]->r_all_w(~m_int_state[cpunum]);
	}
}

WRITE_LINE_MEMBER( cmi_state::maincpu1_irq_w )
{
	LOG("%s: maincpu1_irq_w: %d\n", machine().describe_context(), state);
	m_maincpu1->set_input_line(M6809_IRQ_LINE, state ? ASSERT_LINE : CLEAR_LINE);
}

WRITE_LINE_MEMBER( cmi_state::maincpu2_irq0_w )
{
	LOG("%s: maincpu2_irq0_w: %d\n", machine().describe_context(), state);
	set_interrupt(CPU_2, 0 ^ 7, state ? ASSERT_LINE : CLEAR_LINE);
}

WRITE_LINE_MEMBER( cmi_state::i8214_1_int_w )
{
	LOG("%s: i8214_1_int_w %d%s\n", machine().describe_context(), state, state ? ", setting IRQ merger bit 0" : "");
	if (state)
	{
		m_lp_int = 1;
		//m_maincpu1->set_input_line(M6809_IRQ_LINE, ASSERT_LINE);
		m_maincpu1_irq_merger->in_w<0>(state);
	}
}

WRITE_LINE_MEMBER( cmi_state::i8214_2_int_w )
{
	LOG("%s: i8214_2_int_w: %d\n", machine().describe_context(), state);
	if (state)
		m_maincpu2->set_input_line(M6809_IRQ_LINE, ASSERT_LINE);
}

WRITE_LINE_MEMBER( cmi_state::i8214_3_int_w )
{
	LOG("%s: i8214_3_int_w %d%s\n", machine().describe_context(), state, state ? ", setting IRQ merger bit 1" : "");
	if (state)
	{
		m_hp_int = 1;
		m_maincpu1_irq_merger->in_w<1>(state);
	}
	//m_hp_int = 1;
	//m_maincpu1->set_input_line(M6809_IRQ_LINE, ASSERT_LINE);
	//m_maincpu1->set_input_line(M6809_IRQ_LINE, state ? ASSERT_LINE : CLEAR_LINE);
}


WRITE_LINE_MEMBER( cmi_state::i8214_3_enlg )
{
	// Not needed?
//  m_hp_int = state;
}

WRITE_LINE_MEMBER( cmi_state::pia_q219_irqa )
{
	set_interrupt(CPU_2, IRQ_TOUCHINT_LEVEL, state);
}

WRITE_LINE_MEMBER( cmi_state::pia_q219_irqb )
{
	set_interrupt(CPU_2, IRQ_PENINT_LEVEL, state);
}


/* E9 - E11 - MSM5832RS */
/*
    A0-A3  = MSM5832 A0-A3
    A4+CA1 = MSM5832 D0
    A5+CB1 = MSM5832 D1
    A6     = MSM5832 D2
    A7     = MSM5832 D3
    B0     = HOLD
    B1     = READ
    B2     = WRITE
    CB2    = OPTO
    B3     = ----
    B4-B7  = PC0-3

    IRQA/B = /RTINT?
*/

uint8_t cmi_state::q133_1_porta_r()
{
	if (BIT(m_q133_pia[0]->b_output(), 1))
	{
		return m_msm5832->data_r() << 4;
	}
	return 0xff;
}

void cmi_state::q133_1_porta_w(uint8_t data)
{
	m_msm5832_addr = data & 0xf;
	m_msm5832->address_w(data & 0x0f);
}

void cmi_state::q133_1_portb_w(uint8_t data)
{
	m_msm5832->hold_w(BIT(data, 0));
	m_msm5832->read_w(BIT(data, 1));
	m_msm5832->write_w(BIT(data, 2));
}

 /*************************************
 *
 *  6551 ACIAs
 *
 *************************************/

//static int kbd_to_cmi;
//static int cmi_to_kbd;

WRITE_LINE_MEMBER( cmi_state::q133_acia_clock )
{
	for (auto &acia : m_q133_acia)
		acia->write_rxc(state);
}

WRITE_LINE_MEMBER( cmi_state::msm5832_irq_w )
{
	set_interrupt(CPU_2, IRQ_RTCINT_LEVEL, state ? ASSERT_LINE : CLEAR_LINE);
}

WRITE_LINE_MEMBER( cmi_state::cmi07_irq )
{
	m_cmi07cpu->set_input_line(INPUT_LINE_IRQ0, state ? ASSERT_LINE : CLEAR_LINE);
}

void cmi_state::machine_reset()
{
	m_cpu1space = &m_maincpu1->space(AS_PROGRAM);
	m_cpu2space = &m_maincpu2->space(AS_PROGRAM);

	m_qfc9_region_ptr = (uint8_t *)m_qfc9_region->base();

	m_int_state[0] = 0;
	m_int_state[1] = 0;

	/* Set 8214 interrupt lines */
	m_i8214[0]->etlg_w(1);
	m_i8214[0]->inte_w(1);
	m_i8214[1]->etlg_w(1);
	m_i8214[1]->inte_w(1);
	m_i8214[2]->etlg_w(1);
	m_i8214[2]->inte_w(1);

	m_hblank_timer->adjust(m_screen->time_until_pos(0, HBLANK_START));

	for (int cpunum = 0; cpunum < 2; ++cpunum)
	{
		address_space *space = (cpunum == CPU_1 ? m_cpu1space : m_cpu2space);

		/* Select A (system) spaces */
		m_cpu_active_space[cpunum] = MAPPING_A;

		m_irq_address[cpunum][0] = space->read_byte(0xfff8);
		m_irq_address[cpunum][1] = space->read_byte(0xfff9);
	}

	// TODO - we need to detect empty disk drive!!
	m_fdc_status |= FDC_STATUS_READY;

	/* CMI-07 */
	m_cmi07_ctrl = 0;
	m_cmi07_base_enable[0] = false;
	m_cmi07_base_enable[1] = false;
	m_cmi07_base_addr = 0;
	m_cmi07cpu->set_input_line(INPUT_LINE_RESET, ASSERT_LINE);

	// SMIDI
	m_midicpu->set_input_line(INPUT_LINE_HALT, ASSERT_LINE);

	m_cmi02_ptm_irq = 0;
	m_cmi02_ptm->set_c2(1);
	m_cmi02_ptm->set_g1(0);
	m_cmi02_ptm->set_g2(0);
	m_cmi02_ptm->set_g3(0);
	m_m6809_bs_hack_cnt[0] = 0;
	m_m6809_bs_hack_cnt[1] = 0;

	//m_midi_ptm[0]->set_g1(1); // /G1 has unknown source, "TIMER 1A /GATE" per schematic
	m_midi_ptm[0]->set_g2(0); // /G2 and /G3 wired to ground per schematic
	m_midi_ptm[0]->set_g3(0);

	m_midi_ptm[1]->set_g1(0); // /G1, /G2, and /G3 wired to ground per schematic
	m_midi_ptm[1]->set_g2(0);
	m_midi_ptm[1]->set_g3(0);

	memset(m_map_sel, 0, 16);

	for (int i = 0; i < 4; i++)
	{
		m_q133_acia[i]->write_dsr(0);
		m_q133_acia[i]->write_dcd(0);
	}

	m_curr_mapinfo[0] = 0x00;
	m_curr_mapinfo[1] = 0x00;
}

void cmi_state::machine_start()
{
	m_q133_rom = (uint8_t *)m_q133_region->base();

	// allocate timers for the built-in two channel timer
	m_map_switch_timer = timer_alloc(TIMER_MAP_SWITCH);
	m_hblank_timer = timer_alloc(TIMER_HBLANK);
	m_jam_timeout_timer = timer_alloc(TIMER_JAM_TIMEOUT);

	m_map_switch_timer->adjust(attotime::never);
	m_hblank_timer->adjust(attotime::never);
	m_jam_timeout_timer->adjust(attotime::never);

	/* Allocate 1kB memory mapping RAM */
	m_map_ram[0] = std::make_unique<uint8_t[]>(0x400);
	m_map_ram[1] = std::make_unique<uint8_t[]>(0x400);

	/* Allocate 256kB for each Q256 RAM card */
	m_q256_ram[0] = std::make_unique<uint8_t[]>(0x40000);
	m_q256_ram[1] = std::make_unique<uint8_t[]>(0x40000);

	/* Allocate 16kB video RAM */
	m_video_ram = std::make_unique<uint8_t[]>(0x4000);

	/* Allocate 512B shared RAM */
	m_shared_ram = std::make_unique<uint8_t[]>(0x200);

	/* Allocate 256B scratch RAM per CPU */
	m_scratch_ram[0] = std::make_unique<uint8_t[]>(0x100);
	m_scratch_ram[1] = std::make_unique<uint8_t[]>(0x100);

	m_msm5832->cs_w(1);
}

WRITE_LINE_MEMBER( cmi_state::cmi_iix_vblank )
{
	if (state)
	{
		/* VSYNC */
		m_q219_pia->cb2_w(1);
		m_q219_pia->cb2_w(0);

		/* LPSTB */
		m_q219_pia->cb1_w(0);
	}
}

static void cmi2x_floppies(device_slot_interface &device)
{
	device.option_add("8dsdd", FLOPPY_8_DSDD);
	device.option_add("8dssd", FLOPPY_8_DSSD);
}

void cmi_state::cmi2x(machine_config &config)
{
	MC6809E(config, m_maincpu1, Q209_CPU_CLOCK);
	m_maincpu1->set_addrmap(AS_PROGRAM, &cmi_state::maincpu1_map);
	m_maincpu1->set_irq_acknowledge_callback(FUNC(cmi_state::cpu1_interrupt_callback));
	config.set_perfect_quantum(m_maincpu1);

	MC6809E(config, m_maincpu2, Q209_CPU_CLOCK);
	m_maincpu2->set_addrmap(AS_PROGRAM, &cmi_state::maincpu2_map);
	m_maincpu2->set_irq_acknowledge_callback(FUNC(cmi_state::cpu2_interrupt_callback));

	ADDRESS_MAP_BANK(config, m_cpu1_periphs).set_options(ENDIANNESS_BIG, 8, 16, 0x1000);
	m_cpu1_periphs->set_addrmap(AS_PROGRAM, &cmi_state::cpu1_periphs_map);

	ADDRESS_MAP_BANK(config, m_cpu2_periphs).set_options(ENDIANNESS_BIG, 8, 16, 0x1000);
	m_cpu2_periphs->set_addrmap(AS_PROGRAM, &cmi_state::cpu2_periphs_map);

	M68000(config, m_midicpu, 20_MHz_XTAL / 2);
	m_midicpu->set_addrmap(AS_PROGRAM, &cmi_state::midicpu_map);

	MC6809E(config, m_cmi07cpu, Q209_CPU_CLOCK); // ?
	m_cmi07cpu->set_addrmap(AS_PROGRAM, &cmi_state::cmi07cpu_map);

	/* video hardware */
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER, rgb_t::green());
	m_screen->set_raw(PIXEL_CLOCK, HTOTAL, HBLANK_END, HBLANK_START, VTOTAL, VBLANK_END, VBLANK_START);
	m_screen->set_screen_update(FUNC(cmi_state::screen_update_cmi2x));
	m_screen->screen_vblank().set(FUNC(cmi_state::cmi_iix_vblank));

	PALETTE(config, m_palette, palette_device::MONOCHROME);

	MSM5832(config, m_msm5832, 32.768_kHz_XTAL);

	I8214(config, m_i8214[0], 1000000); // cmi_8214_intf_1
	m_i8214[0]->int_wr_callback().set(FUNC(cmi_state::i8214_1_int_w));
	I8214(config, m_i8214[1], 1000000); // cmi_8214_intf_2
	m_i8214[1]->int_wr_callback().set(FUNC(cmi_state::i8214_2_int_w));
	I8214(config, m_i8214[2], 1000000); // cmi_8214_intf_3
	m_i8214[2]->int_wr_callback().set(FUNC(cmi_state::i8214_3_int_w));
	m_i8214[2]->enlg_wr_callback().set(FUNC(cmi_state::i8214_3_enlg));

	INPUT_MERGER_ANY_HIGH(config, m_maincpu1_irq_merger).output_handler().set(FUNC(cmi_state::maincpu1_irq_w));
	INPUT_MERGER_ANY_HIGH(config, m_maincpu2_irq0_merger).output_handler().set(FUNC(cmi_state::maincpu2_irq0_w));

	PIA6821(config, m_q133_pia[0]); // pia_q133_1_config
	m_q133_pia[0]->readpa_handler().set(FUNC(cmi_state::q133_1_porta_r));
	m_q133_pia[0]->writepa_handler().set(FUNC(cmi_state::q133_1_porta_w));
	m_q133_pia[0]->writepb_handler().set(FUNC(cmi_state::q133_1_portb_w));
	m_q133_pia[0]->irqa_handler().set("rtc_irq_merger", FUNC(input_merger_device::in_w<0>));
	m_q133_pia[0]->irqb_handler().set("rtc_irq_merger", FUNC(input_merger_device::in_w<1>));

	INPUT_MERGER_ANY_HIGH(config, "rtc_irq_merger").output_handler().set(FUNC(cmi_state::msm5832_irq_w));

	PIA6821(config, m_q133_pia[1]); // pia_q133_2_config

	PTM6840(config, m_q133_ptm, SYSTEM_CAS_CLOCK); // ptm_q133_config, clock likely not accurate
	m_q133_ptm->set_external_clocks(1024, 1, 111); // Third is todo
	m_q133_ptm->irq_callback().set(FUNC(cmi_state::q133_ptm_irq_w));

	PIA6821(config, m_q219_pia); // pia_q219_config
	m_q219_pia->readpb_handler().set(FUNC(cmi_state::pia_q219_b_r));
	m_q219_pia->writepa_handler().set(FUNC(cmi_state::vscroll_w));
	m_q219_pia->writepb_handler().set(FUNC(cmi_state::video_attr_w));
	m_q219_pia->irqa_handler().set(FUNC(cmi_state::pia_q219_irqa));
	m_q219_pia->irqb_handler().set(FUNC(cmi_state::pia_q219_irqb));

	PTM6840(config, m_q219_ptm, SYSTEM_CAS_CLOCK); // ptm_q219_config
	m_q219_ptm->set_external_clocks(HBLANK_FREQ.dvalue(), VBLANK_FREQ.dvalue(), SYSTEM_CAS_CLOCK.dvalue() / 2.0);
	m_q219_ptm->irq_callback().set(FUNC(cmi_state::ptm_q219_irq));

	PIA6821(config, m_cmi02_pia[0]); // pia_cmi02_1_config
	m_cmi02_pia[0]->writepa_handler().set(FUNC(cmi_state::cmi02_chsel_w));
	m_cmi02_pia[0]->writepb_handler().set(FUNC(cmi_state::master_tune_w));

	PIA6821(config, m_cmi02_pia[1]); // pia_cmi02_2_config
	m_cmi02_pia[1]->irqa_handler().set(FUNC(cmi_state::cmi02_pia2_irqa_w));
	m_cmi02_pia[1]->ca1_w(0);
	m_cmi02_pia[1]->cb2_handler().set(FUNC(cmi_state::cmi02_pia2_cb2_w));

	PTM6840(config, m_cmi02_ptm, SYSTEM_CAS_CLOCK);
	m_cmi02_ptm->set_external_clocks(0, 0, 0);
	m_cmi02_ptm->o2_callback().set(FUNC(cmi_state::cmi02_ptm_o2));
	m_cmi02_ptm->irq_callback().set(FUNC(cmi_state::cmi02_ptm_irq));

	clock_device &q133_acia_clock(CLOCK(config, "q133_acia_clock", 1.8432_MHz_XTAL / 12));
	q133_acia_clock.signal_handler().set(FUNC(cmi_state::q133_acia_clock));

	for (auto &acia : m_q133_acia)
		MOS6551(config, acia, 1.8432_MHz_XTAL).set_xtal(1.8432_MHz_XTAL);
	m_q133_acia[0]->irq_handler().set("q133_acia_irq", FUNC(input_merger_device::in_w<0>));
	m_q133_acia[1]->irq_handler().set("q133_acia_irq", FUNC(input_merger_device::in_w<1>));
	m_q133_acia[2]->irq_handler().set("q133_acia_irq", FUNC(input_merger_device::in_w<2>));
	m_q133_acia[3]->irq_handler().set("q133_acia_irq", FUNC(input_merger_device::in_w<3>));

	INPUT_MERGER_ANY_HIGH(config, "q133_acia_irq").output_handler().set(FUNC(cmi_state::q133_acia_irq));

	m_q133_acia[0]->txd_handler().set("mkbd", FUNC(cmi_music_keyboard_device::cmi_rxd_w));
	m_q133_acia[0]->rts_handler().set("mkbd", FUNC(cmi_music_keyboard_device::cmi_cts_w));

	// Musical keyboard
	cmi_music_keyboard_device &mkbd(CMI_MUSIC_KEYBOARD(config, "mkbd"));
	mkbd.cmi_txd_handler().set(m_q133_acia[0], FUNC(mos6551_device::write_rxd));
	mkbd.cmi_rts_handler().set(m_q133_acia[0], FUNC(mos6551_device::write_cts));
	mkbd.kbd_txd_handler().set("alphakeys", FUNC(cmi_alphanumeric_keyboard_device::rxd_w));
	mkbd.kbd_rts_handler().set("alphakeys", FUNC(cmi_alphanumeric_keyboard_device::cts_w));

	// Alphanumeric keyboard
	cmi_alphanumeric_keyboard_device &alphakeys(CMI_ALPHANUMERIC_KEYBOARD(config, "alphakeys"));
	alphakeys.txd_handler().set("mkbd", FUNC(cmi_music_keyboard_device::kbd_rxd_w));
	alphakeys.rts_handler().set("mkbd", FUNC(cmi_music_keyboard_device::kbd_cts_w));

	PTM6840(config, m_cmi07_ptm, 2000000); // ptm_cmi07_config
	m_cmi07_ptm->irq_callback().set(FUNC(cmi_state::cmi07_irq));

	PTM6840(config, m_midi_ptm[0], 0);
	m_midi_ptm[0]->set_external_clocks(0, 384000, 0); // C1 is 0, C2 is 384kHz per schematic block diagram, C3 is CLICK SYNC IN
	//m_midi_ptm[0]->o1_callback().set(FUNC(cmi_state::midi_ptm0_c1_w)); // TIMER 1A O/P per schematic
	//m_midi_ptm[0]->o2_callback().set(FUNC(cmi_state::midi_ptm0_c2_w)); // CLK 2 per schematic
	m_midi_ptm[0]->o3_callback().set(FUNC(cmi_state::midi_ptm0_c3_w));

	PTM6840(config, m_midi_ptm[1], 0); // entirely clocked by PTM 0
	//m_midi_ptm[1]->o1_callback().set(FUNC(cmi_state::midi_sync_out_1_w)); // SYNC OUT 1 per schematic
	//m_midi_ptm[1]->o2_callback().set(FUNC(cmi_state::midi_sync_out_2_w)); // SYNC OUT 2 per schematic
	//m_midi_ptm[1]->o3_callback().set(FUNC(cmi_state::midi_sync_out_3_w)); // SYNC OUT 3 per schematic

	for (int i = 0; i < 4; i++)
	{
		ACIA6850(config, m_midi_acia[i]);
		m_midi_acia[i]->txd_handler().set(m_midi_out[i], FUNC(midi_port_device::write_txd));

		MIDI_PORT(config, m_midi_out[i]);
		midiout_slot(*m_midi_out[i]);
	}

	for (int i = 0; i < 3; i++)
	{
		MIDI_PORT(config, m_midi_in[i]);
		midiin_slot(*m_midi_in[i]);
		m_midi_in[i]->rxd_handler().set(m_midi_acia[i], FUNC(acia6850_device::write_rxd));
	}

	INPUT_MERGER_ANY_HIGH(config, "midi_ptm_irq").output_handler().set(FUNC(cmi_state::midi_ptm_irq));
	m_midi_ptm[0]->irq_callback().set("midi_ptm_irq", FUNC(input_merger_device::in_w<0>));
	m_midi_ptm[1]->irq_callback().set("midi_ptm_irq", FUNC(input_merger_device::in_w<1>));

	INPUT_MERGER_ANY_HIGH(config, "midi_acia_irq").output_handler().set(FUNC(cmi_state::midi_acia_irq));
	m_midi_acia[0]->irq_handler().set("midi_acia_irq", FUNC(input_merger_device::in_w<0>));
	m_midi_acia[1]->irq_handler().set("midi_acia_irq", FUNC(input_merger_device::in_w<1>));
	m_midi_acia[2]->irq_handler().set("midi_acia_irq", FUNC(input_merger_device::in_w<2>));
	m_midi_acia[3]->irq_handler().set("midi_acia_irq", FUNC(input_merger_device::in_w<3>));

	FD1791(config, m_wd1791, 16_MHz_XTAL / 8); // wd1791_interface
	m_wd1791->intrq_wr_callback().set(FUNC(cmi_state::wd1791_irq));
	m_wd1791->drq_wr_callback().set(FUNC(cmi_state::wd1791_drq));
	FLOPPY_CONNECTOR(config, "wd1791:0", cmi2x_floppies, "8dsdd", floppy_image_device::default_mfm_floppy_formats);
	FLOPPY_CONNECTOR(config, "wd1791:1", cmi2x_floppies, "8dsdd", floppy_image_device::default_mfm_floppy_formats);

	SPEAKER(config, "mono").front_center();

	// Channel cards
	cmi01a_device &cmi01a_0(CMI01A_CHANNEL_CARD(config, "cmi01a_0", SYSTEM_CAS_CLOCK, 0));
	cmi01a_0.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_0.irq_callback().set(FUNC(cmi_state::channel_irq<0>));
	cmi01a_device &cmi01a_1(CMI01A_CHANNEL_CARD(config, "cmi01a_1", SYSTEM_CAS_CLOCK, 1));
	cmi01a_1.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_1.irq_callback().set(FUNC(cmi_state::channel_irq<1>));
	cmi01a_device &cmi01a_2(CMI01A_CHANNEL_CARD(config, "cmi01a_2", SYSTEM_CAS_CLOCK, 2));
	cmi01a_2.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_2.irq_callback().set(FUNC(cmi_state::channel_irq<2>));
	cmi01a_device &cmi01a_3(CMI01A_CHANNEL_CARD(config, "cmi01a_3", SYSTEM_CAS_CLOCK, 3));
	cmi01a_3.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_3.irq_callback().set(FUNC(cmi_state::channel_irq<3>));
	cmi01a_device &cmi01a_4(CMI01A_CHANNEL_CARD(config, "cmi01a_4", SYSTEM_CAS_CLOCK, 4));
	cmi01a_4.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_4.irq_callback().set(FUNC(cmi_state::channel_irq<4>));
	cmi01a_device &cmi01a_5(CMI01A_CHANNEL_CARD(config, "cmi01a_5", SYSTEM_CAS_CLOCK, 5));
	cmi01a_5.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_5.irq_callback().set(FUNC(cmi_state::channel_irq<5>));
	cmi01a_device &cmi01a_6(CMI01A_CHANNEL_CARD(config, "cmi01a_6", SYSTEM_CAS_CLOCK, 6));
	cmi01a_6.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_6.irq_callback().set(FUNC(cmi_state::channel_irq<6>));
	cmi01a_device &cmi01a_7(CMI01A_CHANNEL_CARD(config, "cmi01a_7", SYSTEM_CAS_CLOCK, 7));
	cmi01a_7.add_route(ALL_OUTPUTS, "mono", 0.12);
	cmi01a_7.irq_callback().set(FUNC(cmi_state::channel_irq<7>));
}

ROM_START( cmi2x )
	/* Q133 Processor control card */
	ROM_REGION( 0x3000, "q133", 0 )
	ROM_LOAD( "q9f0mrk1.bin", 0x000, 0x800, CRC(16f195cc) SHA1(fcc4be370ba60ae5a4145c36cdbdc97a7be91f8f) )
	ROM_LOAD( "f8lmrk5.bin",  0x800, 0x800, CRC(cfc7967f) SHA1(0695cc757cf6fab35414dc068dd2a3e50084685c) )

	/* For CPU1 */
	ROM_COPY( "q133", 0x000, 0x1000, 0x800 )
	ROM_COPY( "q133", 0x800, 0x1800, 0x400 )

	/* For CPU2 */
	ROM_COPY( "q133", 0x000, 0x2000, 0x800 )
	ROM_COPY( "q133", 0xc00, 0x2800, 0x400 )

	/* General Interface (SMPTE/MIDI) CPU */
	ROM_REGION( 0x4000, "smptemidi", 0 )
	ROM_LOAD16_BYTE( "mon1110e.bin", 0x0000, 0x2000, CRC(476f7d5f) SHA1(9af21e0072eaa58cae42947c20dca05d35dfadd0) )
	ROM_LOAD16_BYTE( "mon1110o.bin", 0x0001, 0x2000, CRC(150c8ebe) SHA1(bbd371bebac29628f60537832d0587e83323ad01) )

	/* QFC9 Floppy disk controller driver */
	ROM_REGION( 0x800, "qfc9", 0 )
	ROM_LOAD( "dqfc911.bin", 0x00, 0x800, CRC(5bc38db2) SHA1(bd840e19e51a336e669c40b9e18cdaf6b3c62a8a) )

	// All of these PROM dumps have been trimmed to size from within a roughly 2x-bigger file.
	// The actual sizes are known from the schematics and the starting address of the actual PROM data was obvious
	// based on repeated data in some of the 256x4 PROMs, but it would be nice to get redumps, in the extremely
	// unlikely event that someone finds a CMI IIx for sale.
	ROM_REGION( 0x420, "proms", 0 )
	ROM_LOAD( "brom.bin",   0x000, 0x100, CRC(3f730d15) SHA1(095df6eee95b9ad6418b910fb5d2ae46913750f9) ) // Unknown use, lightgun/graphics card
	ROM_LOAD( "srom.bin",   0x100, 0x100, CRC(a1b4b71b) SHA1(6ea96480af2f1e43967f209218a74fc17972ce0e) ) // Used to generate signal timing for lightpen
	ROM_LOAD( "mrom.bin",   0x200, 0x100, CRC(dc26642c) SHA1(49b207ff80d1b055c3b855dc954129846c49bfe3) ) // Unknown use, master card
	ROM_LOAD( "timrom.bin", 0x300, 0x100, CRC(a426e4a2) SHA1(6b7ea128c730f5afd1042820ccd55bbda683afd8) ) // Unknown use, master card
	ROM_LOAD( "wrom.bin",   0x400, 0x020, CRC(68a9e17f) SHA1(c3364a37a8d19a1882d7910add1c1df9b63ee32c) ) // Unknown use, lightgun/graphics card
ROM_END

/* TODO: Machine start? */
void cmi_state::init_cmi2x()
{
}

CONS( 1983, cmi2x, 0, 0, cmi2x, cmi2x, cmi_state, init_cmi2x, "Fairlight", "CMI IIx", MACHINE_NOT_WORKING | MACHINE_NO_SOUND )
