// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stock amdxdna surucusunun boot dizisini birebir taklit eden test kosumu.
 *
 * Buradaki "surucu" kodu kasten emulatorun ic yapilarina bakmaz: sadece
 * MMIO okur/yazar, tipki gercek surucunun yaptigi gibi. Adimlar ve bekleme
 * kosullari aie2_pci.c / aie_psp.c / aie_smu.c / amdxdna_mailbox.c
 * fonksiyonlarindan birebir turetilmistir.
 *
 * Bu test gecerse: emulator, stock surucunun probe yolunu bastan sona
 * gecirebiliyor demektir (yol haritasi asama 2 ve 4).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xdna/xdna_emu.h"
#include "xdna/xdna_regs.h"

/* ---------------------------------------------------------------- */
/* Kucuk test cercevesi                                              */
/* ---------------------------------------------------------------- */

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("  [BASARISIZ] %s:%d: ", __FILE__, __LINE__);              \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b, name)                                                  \
    do {                                                                      \
        uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);                      \
        CHECK(_a == _b, "%s: 0x%llx beklendi, 0x%llx bulundu", name,          \
              (unsigned long long)_b, (unsigned long long)_a);                \
    } while (0)

static void step(const char *msg)
{
    printf("* %s\n", msg);
}

/* ---------------------------------------------------------------- */
/* Sahte host bellegi + host ops                                     */
/* ---------------------------------------------------------------- */

#define HOST_MEM_BASE 0x100000000ULL
#define HOST_MEM_SIZE (8u * 1024u * 1024u)

typedef struct {
    uint8_t *mem;
    unsigned irq_count;
    unsigned last_vector;
    bool irq_pending[XDNA_MSIX_VECTORS];
    bool verbose;
} Host;

static int host_dma_read(void *opaque, uint64_t addr, void *buf, size_t len)
{
    Host *h = opaque;

    if (addr < HOST_MEM_BASE || addr + len > HOST_MEM_BASE + HOST_MEM_SIZE) {
        return -1;
    }
    memcpy(buf, h->mem + (addr - HOST_MEM_BASE), len);
    return 0;
}

static int host_dma_write(void *opaque, uint64_t addr, const void *buf,
                          size_t len)
{
    Host *h = opaque;

    if (addr < HOST_MEM_BASE || addr + len > HOST_MEM_BASE + HOST_MEM_SIZE) {
        return -1;
    }
    memcpy(h->mem + (addr - HOST_MEM_BASE), buf, len);
    return 0;
}

static void host_raise_irq(void *opaque, unsigned vector)
{
    Host *h = opaque;

    h->irq_count++;
    h->last_vector = vector;
    if (vector < XDNA_MSIX_VECTORS) {
        h->irq_pending[vector] = true;
    }
}

static void host_log(void *opaque, int level, const char *msg)
{
    Host *h = opaque;
    static const char *names[] = { "HATA", "UYARI", "BILGI", "AYIKLA" };

    if (!h->verbose && level > XDNA_LOG_INFO) {
        return;
    }
    printf("    [%s] %s\n", names[level < 4 ? level : 3], msg);
}

static const XdnaHostOps host_ops = {
    .dma_read = host_dma_read,
    .dma_write = host_dma_write,
    .raise_irq = host_raise_irq,
    .log = host_log,
};

/* ---------------------------------------------------------------- */
/* Surucu modeli                                                     */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t rb_start;      /* SRAM BAR offseti */
    uint32_t rb_size;
    uint32_t head_reg;      /* MBOX BAR offseti */
    uint32_t tail_reg;
} ChannRes;

typedef struct {
    XdnaNpu *npu;
    Host *host;

    ChannRes x2i;
    ChannRes i2x;
    uint32_t intr_reg;
    uint32_t msix_id;
    uint32_t prot_major, prot_minor;

    uint32_t x2i_tail;      /* surucunun kendi kopyasi */
    uint32_t i2x_head;
    uint32_t next_id;
} Drv;

static uint32_t rd32(Drv *d, int bar, uint32_t off)
{
    return (uint32_t)xdna_npu_mmio_read(d->npu, bar, off, 4);
}

static void wr32(Drv *d, int bar, uint32_t off, uint32_t val)
{
    xdna_npu_mmio_write(d->npu, bar, off, val, 4);
}

/* --- PSP (aie_psp.c psp_exec) --- */

static int psp_exec(Drv *d, uint32_t cmd, uint32_t arg0, uint32_t arg1,
                    uint32_t arg2)
{
    uint32_t status, resp;

    status = rd32(d, XDNA_BAR_PSP, XDNA_PSP_STATUS_REG);
    if (!(status & PSP_STATUS_READY)) {
        printf("  [BASARISIZ] PSP komut oncesi hazir degil (0x%x)\n", status);
        g_failures++;
        return -1;
    }

    /* PSP_SET_CMD: arg2 |= cmd << 24, sonra arg2_mask uygulanir */
    arg2 = (arg2 | (cmd << PSP_ARG2_CMD_SHIFT)) & PSP_ARG2_MASK;

    wr32(d, XDNA_BAR_PSP, XDNA_PSP_CMD_REG, cmd);
    wr32(d, XDNA_BAR_PSP, XDNA_PSP_ARG0_REG, arg0);
    wr32(d, XDNA_BAR_PSP, XDNA_PSP_ARG1_REG, arg1);
    wr32(d, XDNA_BAR_PSP, XDNA_PSP_ARG2_REG, arg2);

    wr32(d, XDNA_BAR_PSP, XDNA_PSP_INTR_REG, 0);
    wr32(d, XDNA_BAR_PSP, XDNA_PSP_INTR_REG, PSP_NOTIFY_VAL);

    status = rd32(d, XDNA_BAR_PSP, XDNA_PSP_STATUS_REG);
    if (!(status & PSP_STATUS_READY)) {
        printf("  [BASARISIZ] PSP komut sonrasi hazir degil (0x%x)\n", status);
        g_failures++;
        return -1;
    }

    resp = rd32(d, XDNA_BAR_PSP, XDNA_PSP_RESP_REG);
    return resp ? -1 : 0;
}

/* --- SMU (aie_smu.c aie_smu_exec) --- */

static int smu_exec(Drv *d, uint32_t cmd, uint32_t arg, uint32_t *out)
{
    uint32_t resp;

    wr32(d, XDNA_BAR_SMU, XDNA_SMU_RESP_REG, 0);
    wr32(d, XDNA_BAR_SMU, XDNA_SMU_ARG_REG, arg);
    wr32(d, XDNA_BAR_SMU, XDNA_SMU_CMD_REG, cmd);
    wr32(d, XDNA_BAR_SMU, XDNA_SMU_INTR_REG, 0);
    wr32(d, XDNA_BAR_SMU, XDNA_SMU_INTR_REG, 1);

    resp = rd32(d, XDNA_BAR_SMU, XDNA_SMU_RESP_REG);
    if (!resp) {
        printf("  [BASARISIZ] SMU komut 0x%x zaman asimi\n", cmd);
        g_failures++;
        return -1;
    }
    if (out) {
        *out = rd32(d, XDNA_BAR_SMU, XDNA_SMU_OUT_REG);
    }
    return resp == SMU_RESULT_OK ? 0 : -1;
}

/* --- Mailbox (amdxdna_mailbox.c) --- */

typedef struct {
    uint32_t total_size;
    uint32_t sz_ver;
    uint32_t id;
    uint32_t opcode;
} MsgHeader;

static int mbox_recv(Drv *d, uint32_t want_id, void *resp, uint32_t resp_size)
{
    MsgHeader hdr;
    uint32_t tail, head, peek, msg_size;
    int guard = 0;

    /* mailbox_irq_acknowledge() */
    wr32(d, XDNA_BAR_MBOX, d->intr_reg, 0);

    for (;;) {
        if (++guard > 64) {
            printf("  [BASARISIZ] i2x tuketiminde dongu\n");
            g_failures++;
            return -1;
        }
        tail = rd32(d, XDNA_BAR_MBOX, d->i2x.tail_reg);
        head = d->i2x_head;
        if (head == tail) {
            printf("  [BASARISIZ] cevap yok (head=tail=%u)\n", head);
            g_failures++;
            return -1;
        }
        if (head == d->i2x.rb_size) {
            head = 0;
        }

        xdna_npu_mmio_read_buf(d->npu, XDNA_BAR_SRAM,
                               d->i2x.rb_start + head, &peek, 4);
        if (peek == XDNA_MBOX_TOMBSTONE) {
            head = 0;
            d->i2x_head = 0;
            wr32(d, XDNA_BAR_MBOX, d->i2x.head_reg, 0);
            continue;
        }
        break;
    }

    xdna_npu_mmio_read_buf(d->npu, XDNA_BAR_SRAM, d->i2x.rb_start + head,
                           &hdr, sizeof(hdr));
    msg_size = (uint32_t)sizeof(hdr) + hdr.total_size;

    /*
     * xdna_msg_cb() esitlik kontrolu: boyut farkli olursa surucu -EINVAL
     * dondurur. Emulatorun en kolay yanlis yapacagi yer burasi.
     */
    CHECK_EQ(hdr.total_size, resp_size, "cevap payload boyutu");
    CHECK_EQ(hdr.id, want_id, "cevap mesaj id");
    CHECK_EQ(hdr.sz_ver >> XDNA_MSG_PROTO_VER_SHIFT & 0xFF,
             XDNA_MSG_PROTOCOL_VER, "mesaj protokol surumu");

    if (hdr.total_size == resp_size && resp_size) {
        xdna_npu_mmio_read_buf(d->npu, XDNA_BAR_SRAM,
                               d->i2x.rb_start + head + sizeof(hdr), resp,
                               resp_size);
    }

    d->i2x_head = head + msg_size;
    wr32(d, XDNA_BAR_MBOX, d->i2x.head_reg, d->i2x_head);
    return 0;
}

static int mbox_send_recv(Drv *d, uint32_t opcode, const void *req,
                          uint32_t req_size, void *resp, uint32_t resp_size)
{
    MsgHeader hdr;
    uint32_t head, tail, usable, pkg_size, id;

    pkg_size = (uint32_t)sizeof(hdr) + req_size;
    head = rd32(d, XDNA_BAR_MBOX, d->x2i.head_reg);
    tail = d->x2i_tail;
    usable = d->x2i.rb_size - 4;

    if (tail >= head && tail + pkg_size > usable) {
        uint32_t tomb = XDNA_MBOX_TOMBSTONE;
        xdna_npu_mmio_write_buf(d->npu, XDNA_BAR_SRAM, d->x2i.rb_start + tail,
                                &tomb, 4);
        tail = 0;
    }
    if (tail < head && tail + pkg_size >= head) {
        printf("  [BASARISIZ] x2i ring dolu\n");
        g_failures++;
        return -1;
    }

    id = (d->next_id++ & 0xFF) | XDNA_MBOX_MAGIC_VAL;
    hdr.total_size = req_size;
    hdr.sz_ver = (req_size & XDNA_MSG_BODY_SZ_MASK) |
                 (XDNA_MSG_PROTOCOL_VER << XDNA_MSG_PROTO_VER_SHIFT);
    hdr.id = id;
    hdr.opcode = opcode;

    xdna_npu_mmio_write_buf(d->npu, XDNA_BAR_SRAM, d->x2i.rb_start + tail,
                            &hdr, sizeof(hdr));
    if (req_size) {
        xdna_npu_mmio_write_buf(d->npu, XDNA_BAR_SRAM,
                                d->x2i.rb_start + tail + sizeof(hdr), req,
                                req_size);
    }

    d->x2i_tail = tail + pkg_size;
    d->host->irq_pending[d->msix_id] = false;
    /* Tail yazimi cihazi tetikler. */
    wr32(d, XDNA_BAR_MBOX, d->x2i.tail_reg, d->x2i_tail);

    CHECK(d->host->irq_pending[d->msix_id],
          "opcode 0x%x icin MSI-X %u tetiklenmedi", opcode, d->msix_id);

    return mbox_recv(d, id, resp, resp_size);
}

/* ---------------------------------------------------------------- */
/* mgmt_mbox_chann_info okumasi (aie2_get_mgmt_chann_info)           */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t x2i_tail, x2i_head, x2i_buf, x2i_buf_sz;
    uint32_t i2x_tail, i2x_head, i2x_buf, i2x_buf_sz;
    uint32_t magic, msi_id, prot_major, prot_minor;
    uint32_t rsvd[4];
} MgmtInfo;

static int drv_get_mgmt_chann_info(Drv *d)
{
    MgmtInfo info;
    uint32_t addr, off;

    addr = rd32(d, XDNA_BAR_SRAM, XDNA_SRAM_FW_ALIVE_OFF);
    if (!addr) {
        printf("  [BASARISIZ] firmware alive degil\n");
        g_failures++;
        return -1;
    }
    off = addr - MPNPU_APERTURE1_BASE;
    xdna_npu_mmio_read_buf(d->npu, XDNA_BAR_SRAM, off, &info, sizeof(info));

    CHECK_EQ(info.magic, XDNA_MGMT_MBOX_MAGIC, "mgmt mailbox magic");
    if (info.magic != XDNA_MGMT_MBOX_MAGIC) {
        return -1;
    }

    d->i2x.head_reg = info.i2x_head - MPNPU_APERTURE2_BASE;
    d->i2x.tail_reg = info.i2x_tail - MPNPU_APERTURE2_BASE;
    d->i2x.rb_start = info.i2x_buf - MPNPU_APERTURE1_BASE;
    d->i2x.rb_size = info.i2x_buf_sz;

    d->x2i.head_reg = info.x2i_head - MPNPU_APERTURE2_BASE;
    d->x2i.tail_reg = info.x2i_tail - MPNPU_APERTURE2_BASE;
    d->x2i.rb_start = info.x2i_buf - MPNPU_APERTURE1_BASE;
    d->x2i.rb_size = info.x2i_buf_sz;

    d->msix_id = info.msi_id;
    d->prot_major = info.prot_major;
    d->prot_minor = info.prot_minor;
    /* aie2_pci.c:387 -- interrupt register i2x head'in hemen ardindadir. */
    d->intr_reg = d->i2x.head_reg + 4;

    /* Surucu FW_ALIVE'i temizler. */
    wr32(d, XDNA_BAR_SRAM, XDNA_SRAM_FW_ALIVE_OFF, 0);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Testler                                                           */
/* ---------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct { uint32_t status, major, minor, sub, build; } FwVerResp;
typedef struct { uint32_t status; uint16_t major, minor; } AieVerResp;
typedef struct {
    uint32_t size;
    uint16_t major, minor, cols, rows, core_rows, mem_rows, shim_rows;
    uint16_t core_row_start, mem_row_start, shim_row_start;
    uint16_t core_dma_channels, mem_dma_channels, shim_dma_channels;
    uint16_t core_locks, mem_locks, shim_locks;
    uint16_t core_events, mem_events, shim_events, reserved;
} TileInfo;
typedef struct { uint32_t status; TileInfo info; } TileInfoResp;
typedef struct { uint32_t status; } StatusResp;
typedef struct { uint32_t type; uint64_t value; } SetRtCfgReq;
typedef struct { uint16_t pasid, reserved; } PasidReq;
typedef struct { uint32_t head_addr, tail_addr, buf_addr, buf_size; } CqInfo;
typedef struct { CqInfo x2i_q, i2x_q; } CqPair;
typedef struct {
    uint32_t aie_type;
    uint8_t start_col, num_col, num_unused_col, reserved;
    uint8_t num_cq_pairs_requested, reserved1;
    uint16_t pasid;
    uint32_t pad[2];
    uint32_t sec_comm_target_type;
    uint32_t context_priority;
} CreateCtxReq;
typedef struct {
    uint32_t status, context_id;
    uint16_t msix_id;
    uint8_t num_cq_pairs_allocated, reserved;
    CqPair cq_pair[2];
} CreateCtxResp;
typedef struct { uint32_t context_id; } DestroyCtxReq;
typedef struct { uint32_t context_id; uint64_t buf_addr, buf_size; } MapBufReq;
#pragma pack(pop)

#define FW_PADDR (HOST_MEM_BASE + 0x10000)
#define FW_SIZE  0x40000u

static int drv_hw_start(Drv *d)
{
    /* aie_smu_init(): once POWER_OFF, sonra POWER_ON */
    if (smu_exec(d, AIE_SMU_POWER_OFF, 0, NULL)) {
        return -1;
    }
    if (smu_exec(d, AIE_SMU_POWER_ON, 0, NULL)) {
        return -1;
    }

    /* aie_psp_start(): VALIDATE, ardindan START(COPY_FW) */
    if (psp_exec(d, PSP_CMD_VALIDATE, (uint32_t)FW_PADDR,
                 (uint32_t)(FW_PADDR >> 32), FW_SIZE)) {
        return -1;
    }
    if (psp_exec(d, PSP_CMD_START, PSP_START_COPY_FW, 0, 0)) {
        return -1;
    }

    return drv_get_mgmt_chann_info(d);
}

static void test_boot(Host *host, XdnaNpu *npu)
{
    Drv drv = { .npu = npu, .host = host, .next_id = 1 };
    Drv *d = &drv;
    StatusResp st;
    FwVerResp fw;
    AieVerResp av;
    TileInfoResp ti;
    CreateCtxResp cctx;
    uint32_t clk;

    step("SMU guc dizisi + PSP firmware yukleme");
    CHECK(drv_hw_start(d) == 0, "hw_start basarisiz");
    if (g_failures) {
        return;
    }

    step("Yonetim kanali ve protokol surumu");
    CHECK_EQ(d->prot_major, XDNA_MGMT_PROT_MAJOR, "protokol major");
    CHECK(d->prot_minor >= 7, "protokol minor >= 7 olmali (npu1 feature tbl)");
    CHECK_EQ(d->x2i.rb_size, XDNA_SRAM_MGMT_RB_SIZE, "x2i ring boyutu");
    CHECK_EQ(d->intr_reg, d->i2x.head_reg + 4, "interrupt register konumu");
    CHECK_EQ(rd32(d, XDNA_BAR_PSP, XDNA_PSP_PWAITMODE_REG) & 1, 1,
             "pwaitmode bit0");

    step("aie2_mgmt_fw_init: runtime config");
    {
        /* npu1_default_rt_cfg[] INIT kategorisi: {2,1} ve {4,1} */
        SetRtCfgReq req = { .type = 2, .value = 1 };
        CHECK(mbox_send_recv(d, MSG_OP_SET_RUNTIME_CONFIG, &req, sizeof(req),
                             &st, sizeof(st)) == 0, "rt cfg 2 gonderilemedi");
        CHECK_EQ(st.status, 0, "rt cfg 2 durumu");
        req.type = 4;
        CHECK(mbox_send_recv(d, MSG_OP_SET_RUNTIME_CONFIG, &req, sizeof(req),
                             &st, sizeof(st)) == 0, "rt cfg 4 gonderilemedi");
        CHECK_EQ(st.status, 0, "rt cfg 4 durumu");
    }

    step("aie2_assign_mgmt_pasid");
    {
        PasidReq req = { .pasid = 0 };
        CHECK(mbox_send_recv(d, MSG_OP_ASSIGN_MGMT_PASID, &req, sizeof(req),
                             &st, sizeof(st)) == 0, "pasid gonderilemedi");
        CHECK_EQ(st.status, 0, "pasid durumu");
    }

    step("aie2_xdna_reset: SUSPEND + RESUME");
    {
        uint32_t ph = 0;
        CHECK(mbox_send_recv(d, MSG_OP_SUSPEND, &ph, sizeof(ph), &st,
                             sizeof(st)) == 0, "suspend gonderilemedi");
        CHECK_EQ(st.status, 0, "suspend durumu");
        CHECK(mbox_send_recv(d, MSG_OP_RESUME, &ph, sizeof(ph), &st,
                             sizeof(st)) == 0, "resume gonderilemedi");
        CHECK_EQ(st.status, 0, "resume durumu");
    }

    step("aie2_mgmt_fw_query: firmware / AIE surumu / tile bilgisi");
    {
        uint32_t rsv = 0;
        CHECK(mbox_send_recv(d, MSG_OP_GET_FIRMWARE_VERSION, &rsv, sizeof(rsv),
                             &fw, sizeof(fw)) == 0, "fw version");
        CHECK_EQ(fw.status, 0, "fw version durumu");
        CHECK_EQ(fw.major, XDNA_FW_VER_MAJOR, "fw major");

        CHECK(mbox_send_recv(d, MSG_OP_QUERY_AIE_VERSION, &rsv, sizeof(rsv),
                             &av, sizeof(av)) == 0, "aie version");
        CHECK_EQ(av.status, 0, "aie version durumu");

        CHECK(mbox_send_recv(d, MSG_OP_QUERY_AIE_TILE_INFO, &rsv, sizeof(rsv),
                             &ti, sizeof(ti)) == 0, "tile info");
        CHECK_EQ(ti.status, 0, "tile info durumu");
        CHECK_EQ(ti.info.cols, XDNA_AIE_COLS, "kolon sayisi");
        CHECK_EQ(ti.info.rows, XDNA_AIE_ROWS, "satir sayisi");
        CHECK_EQ(ti.info.core_rows, XDNA_AIE_CORE_ROWS, "compute satirlari");
    }

    step("SMU DPM / saat ayarlari");
    {
        clk = 400;
        CHECK(smu_exec(d, AIE_SMU_SET_MPNPUCLK_FREQ, clk, &clk) == 0,
              "mpnpuclk");
        CHECK_EQ(clk, 400, "mpnpuclk geri okuma");
        clk = 800;
        CHECK(smu_exec(d, AIE_SMU_SET_HCLK_FREQ, clk, &clk) == 0, "hclk");
        CHECK_EQ(clk, 800, "hclk geri okuma");
        CHECK(smu_exec(d, AIE_SMU_SET_HARD_DPMLEVEL, 7, NULL) == 0, "hard dpm");
        CHECK(smu_exec(d, AIE_SMU_SET_SOFT_DPMLEVEL, 7, NULL) == 0, "soft dpm");
        CHECK(smu_exec(d, AIE_SMU_SET_HARD_DPMLEVEL, 99, NULL) != 0,
              "gecersiz DPM seviyesi reddedilmeli");
    }

    step("Hardware context olustur / heap bagla / yok et");
    {
        CreateCtxReq req = {
            .aie_type = 1,
            .start_col = 0,
            .num_col = 4,
            .num_cq_pairs_requested = 1,
            .pasid = 13,
            .context_priority = 3,
        };
        MapBufReq map;
        DestroyCtxReq dreq;

        CHECK(mbox_send_recv(d, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                             &cctx, sizeof(cctx)) == 0, "create context");
        CHECK_EQ(cctx.status, 0, "create context durumu");
        CHECK(cctx.context_id != 0, "context id sifir olmamali");
        CHECK_EQ(cctx.num_cq_pairs_allocated, 1, "cq pair sayisi");
        CHECK(cctx.msix_id != XDNA_MGMT_MSIX_VECTOR,
              "context MSI-X vektoru mgmt ile cakismamali");
        CHECK_EQ(cctx.cq_pair[0].i2x_q.head_addr + 4,
                 MPNPU_APERTURE2_BASE +
                     XDNA_MBOX_CHAN_BASE(cctx.msix_id) + XDNA_MBOX_INTR_OFF,
                 "context interrupt register konumu");
        CHECK(cctx.cq_pair[0].x2i_q.buf_addr >= MPNPU_APERTURE1_BASE,
              "x2i buffer SRAM aperture icinde olmali");

        map.context_id = cctx.context_id;
        map.buf_addr = HOST_MEM_BASE + 0x200000;
        map.buf_size = 0x100000;
        CHECK(mbox_send_recv(d, MSG_OP_MAP_HOST_BUFFER, &map, sizeof(map), &st,
                             sizeof(st)) == 0, "map host buffer");
        CHECK_EQ(st.status, 0, "map host buffer durumu");

        dreq.context_id = cctx.context_id;
        CHECK(mbox_send_recv(d, MSG_OP_DESTROY_CONTEXT, &dreq, sizeof(dreq),
                             &st, sizeof(st)) == 0, "destroy context");
        CHECK_EQ(st.status, 0, "destroy context durumu");
    }

    step("Context limiti (hwctx_limit = 6)");
    {
        CreateCtxReq req = {
            .aie_type = 1, .num_col = 1, .num_cq_pairs_requested = 1,
            .context_priority = 3,
        };
        uint32_t ids[XDNA_NPU1_HWCTX_LIMIT];
        unsigned i;

        for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
            CHECK(mbox_send_recv(d, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                                 &cctx, sizeof(cctx)) == 0, "ctx olusturma");
            CHECK_EQ(cctx.status, 0, "ctx durumu");
            ids[i] = cctx.context_id;
        }
        CHECK(mbox_send_recv(d, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                             &cctx, sizeof(cctx)) == 0, "7. ctx cevabi");
        CHECK(cctx.status != 0, "7. context reddedilmeli");

        for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
            DestroyCtxReq dreq = { .context_id = ids[i] };
            CHECK(mbox_send_recv(d, MSG_OP_DESTROY_CONTEXT, &dreq,
                                 sizeof(dreq), &st, sizeof(st)) == 0,
                  "ctx yok etme");
            CHECK_EQ(st.status, 0, "ctx yok etme durumu");
        }
    }

    step("Ring buffer sarmalanmasi (TOMBSTONE yolu, her iki yon)");
    {
        uint32_t rsv = 0;
        unsigned i;
        unsigned wraps_before = 0;

        (void)wraps_before;
        for (i = 0; i < 300; i++) {
            if (mbox_send_recv(d, MSG_OP_GET_FIRMWARE_VERSION, &rsv,
                               sizeof(rsv), &fw, sizeof(fw)) != 0) {
                printf("  [BASARISIZ] %u. mesajda sarmalama hatasi\n", i);
                break;
            }
            if (fw.status != 0) {
                CHECK_EQ(fw.status, 0, "sarmalama sirasinda fw durumu");
                break;
            }
        }
        CHECK(i == 300, "300 mesajin tamami islenmeli (islenen: %u)", i);
    }

    step("Array asamasi henuz yok: exec mesajlari acikca reddedilmeli");
    {
        uint8_t req[80] = { 0 };
        CHECK(mbox_send_recv(d, MSG_OP_EXECUTE_BUFFER_CF, req, sizeof(req),
                             &st, sizeof(st)) == 0, "execbuf cevabi");
        CHECK(st.status != 0, "execbuf sessizce basarili donmemeli");
    }
}

static void test_error_paths(Host *host, XdnaNpu *npu)
{
    Drv drv = { .npu = npu, .host = host, .next_id = 1 };
    Drv *d = &drv;

    step("VALIDATE yapilmadan START reddedilmeli");
    xdna_npu_reset(npu);
    CHECK(psp_exec(d, PSP_CMD_START, PSP_START_COPY_FW, 0, 0) != 0,
          "dogrulanmamis firmware baslatilmamali");
    CHECK_EQ(rd32(d, XDNA_BAR_SRAM, XDNA_SRAM_FW_ALIVE_OFF), 0,
             "firmware alive olmamali");

    step("Erisilemeyen firmware adresi reddedilmeli");
    CHECK(psp_exec(d, PSP_CMD_VALIDATE, 0xDEAD0000u, 0xFFFFu, 0x1000) != 0,
          "gecersiz firmware adresi kabul edilmemeli");

    step("Suspend/resume dongusu: RELEASE_TMR sonrasi yeniden boot");
    xdna_npu_reset(npu);
    CHECK(drv_hw_start(d) == 0, "ilk boot");
    CHECK(psp_exec(d, PSP_CMD_RELEASE_TMR, 0, 0, 0) == 0, "release tmr");
    CHECK_EQ(rd32(d, XDNA_BAR_SRAM, XDNA_SRAM_FW_ALIVE_OFF), 0,
             "release sonrasi fw alive temizlenmeli");
    CHECK(smu_exec(d, AIE_SMU_POWER_OFF, 0, NULL) == 0, "power off");

    d->next_id = 1;
    CHECK(drv_hw_start(d) == 0, "ikinci boot");
    {
        uint32_t rsv = 0;
        FwVerResp fw;
        CHECK(mbox_send_recv(d, MSG_OP_GET_FIRMWARE_VERSION, &rsv, sizeof(rsv),
                             &fw, sizeof(fw)) == 0, "yeniden boot sonrasi fw");
        CHECK_EQ(fw.status, 0, "yeniden boot sonrasi fw durumu");
    }
}

int main(int argc, char **argv)
{
    Host host = { 0 };
    XdnaNpu *npu;

    host.verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    host.mem = calloc(1, HOST_MEM_SIZE);
    if (!host.mem) {
        return 1;
    }
    /* Sahte npu.sbin imaji. */
    memset(host.mem + (FW_PADDR - HOST_MEM_BASE), 0xA5, FW_SIZE);

    npu = xdna_npu_new(&host_ops, &host);
    if (!npu) {
        free(host.mem);
        return 1;
    }

    printf("=== XDNA1 emulatoru: stock amdxdna boot dizisi ===\n");
    test_boot(&host, npu);

    printf("\n=== Hata yollari ve yeniden boot ===\n");
    test_error_paths(&host, npu);

    {
        XdnaStats stats;
        xdna_npu_get_stats(npu, &stats);
        printf("\nIstatistik: psp=%llu smu=%llu mbox_in=%llu mbox_out=%llu "
               "irq=%llu\n",
               (unsigned long long)stats.psp_cmds,
               (unsigned long long)stats.smu_cmds,
               (unsigned long long)stats.mbox_msgs_in,
               (unsigned long long)stats.mbox_msgs_out,
               (unsigned long long)stats.irqs_raised);
    }

    xdna_npu_free(npu);
    free(host.mem);

    printf("\n%d kontrol, %d basarisiz\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
