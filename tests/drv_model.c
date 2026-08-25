// SPDX-License-Identifier: GPL-2.0-only
/* Stock amdxdna surucu davranis modeli -- bkz. drv_model.h */

#include <stdlib.h>
#include <string.h>

#include "drv_model.h"

int g_failures;
int g_checks;

void step(const char *msg)
{
    printf("* %s\n", msg);
}

/* ---------------------------------------------------------------- */
/* Host ops                                                          */
/* ---------------------------------------------------------------- */

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

const XdnaHostOps drv_host_ops = {
    .dma_read = host_dma_read,
    .dma_write = host_dma_write,
    .raise_irq = host_raise_irq,
    .log = host_log,
};

Host *host_new(bool verbose)
{
    Host *h = calloc(1, sizeof(*h));

    if (!h) {
        return NULL;
    }
    h->mem = calloc(1, HOST_MEM_SIZE);
    if (!h->mem) {
        free(h);
        return NULL;
    }
    h->verbose = verbose;
    /* Sahte npu.sbin imaji */
    memset(h->mem + (FW_PADDR - HOST_MEM_BASE), 0xA5, FW_SIZE);
    return h;
}

void host_free(Host *h)
{
    if (!h) {
        return;
    }
    free(h->mem);
    free(h);
}

uint8_t *host_ptr(Host *h, uint64_t addr)
{
    if (addr < HOST_MEM_BASE || addr >= HOST_MEM_BASE + HOST_MEM_SIZE) {
        return NULL;
    }
    return h->mem + (addr - HOST_MEM_BASE);
}

/* ---------------------------------------------------------------- */
/* MMIO                                                              */
/* ---------------------------------------------------------------- */

uint32_t rd32(Drv *d, int bar, uint32_t off)
{
    return (uint32_t)xdna_npu_mmio_read(d->npu, bar, off, 4);
}

void wr32(Drv *d, int bar, uint32_t off, uint32_t val)
{
    xdna_npu_mmio_write(d->npu, bar, off, val, 4);
}

/* ---------------------------------------------------------------- */
/* PSP / SMU                                                         */
/* ---------------------------------------------------------------- */

int psp_exec(Drv *d, uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    uint32_t status, resp;

    status = rd32(d, XDNA_BAR_PSP, XDNA_PSP_STATUS_REG);
    if (!(status & PSP_STATUS_READY)) {
        printf("  [BASARISIZ] PSP komut oncesi hazir degil (0x%x)\n", status);
        g_failures++;
        return -1;
    }

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

int smu_exec(Drv *d, uint32_t cmd, uint32_t arg, uint32_t *out)
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

/* ---------------------------------------------------------------- */
/* Mailbox                                                           */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t total_size;
    uint32_t sz_ver;
    uint32_t id;
    uint32_t opcode;
} MsgHeader;

int mbox_recv_pending(Drv *d, uint32_t want_id, void *resp, uint32_t resp_size)
{
    MsgHeader hdr;
    uint32_t tail, head, peek, msg_size;
    int guard = 0;

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

    /* xdna_msg_cb() esitlik kontrolu */
    CHECK_EQ(hdr.total_size, resp_size, "cevap payload boyutu");
    CHECK_EQ(hdr.id, want_id, "cevap mesaj id");
    CHECK_EQ((hdr.sz_ver >> XDNA_MSG_PROTO_VER_SHIFT) & 0xFF,
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

/* Ortak gonderme yolu; cevap beklemez. */
static uint32_t mbox_post(Drv *d, uint32_t opcode, const void *req,
                          uint32_t req_size)
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
        return 0;
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
    wr32(d, XDNA_BAR_MBOX, d->x2i.tail_reg, d->x2i_tail);
    return id;
}

uint32_t mbox_send_only(Drv *d, uint32_t opcode, const void *req,
                        uint32_t req_size)
{
    return mbox_post(d, opcode, req, req_size);
}

int mbox_send_recv(Drv *d, uint32_t opcode, const void *req, uint32_t req_size,
                   void *resp, uint32_t resp_size)
{
    uint32_t id = mbox_post(d, opcode, req, req_size);

    if (!id) {
        return -1;
    }

    CHECK(d->host->irq_pending[d->msix_id],
          "opcode 0x%x icin MSI-X %u tetiklenmedi", opcode, d->msix_id);

    return mbox_recv_pending(d, id, resp, resp_size);
}

/* ---------------------------------------------------------------- */
/* Boot                                                              */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t x2i_tail, x2i_head, x2i_buf, x2i_buf_sz;
    uint32_t i2x_tail, i2x_head, i2x_buf, i2x_buf_sz;
    uint32_t magic, msi_id, prot_major, prot_minor;
    uint32_t rsvd[4];
} MgmtInfo;

int drv_get_mgmt_chann_info(Drv *d)
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
    d->intr_reg = d->i2x.head_reg + 4;

    wr32(d, XDNA_BAR_SRAM, XDNA_SRAM_FW_ALIVE_OFF, 0);
    return 0;
}

int drv_hw_start(Drv *d)
{
    if (smu_exec(d, AIE_SMU_POWER_OFF, 0, NULL)) {
        return -1;
    }
    if (smu_exec(d, AIE_SMU_POWER_ON, 0, NULL)) {
        return -1;
    }
    if (psp_exec(d, PSP_CMD_VALIDATE, (uint32_t)FW_PADDR,
                 (uint32_t)(FW_PADDR >> 32), FW_SIZE)) {
        return -1;
    }
    if (psp_exec(d, PSP_CMD_START, PSP_START_COPY_FW, 0, 0)) {
        return -1;
    }
    return drv_get_mgmt_chann_info(d);
}

void drv_attach_ctx_channel(const CreateCtxResp *resp, Drv *out, Drv *mgmt)
{
    const CqInfo *x2i = &resp->cq_pair[0].x2i_q;
    const CqInfo *i2x = &resp->cq_pair[0].i2x_q;

    memset(out, 0, sizeof(*out));
    out->npu = mgmt->npu;
    out->host = mgmt->host;
    out->next_id = 1;

    out->x2i.head_reg = x2i->head_addr - MPNPU_APERTURE2_BASE;
    out->x2i.tail_reg = x2i->tail_addr - MPNPU_APERTURE2_BASE;
    out->x2i.rb_start = x2i->buf_addr - MPNPU_APERTURE1_BASE;
    out->x2i.rb_size = x2i->buf_size;

    out->i2x.head_reg = i2x->head_addr - MPNPU_APERTURE2_BASE;
    out->i2x.tail_reg = i2x->tail_addr - MPNPU_APERTURE2_BASE;
    out->i2x.rb_start = i2x->buf_addr - MPNPU_APERTURE1_BASE;
    out->i2x.rb_size = i2x->buf_size;

    out->intr_reg = out->i2x.head_reg + 4;
    out->msix_id = resp->msix_id;
}
