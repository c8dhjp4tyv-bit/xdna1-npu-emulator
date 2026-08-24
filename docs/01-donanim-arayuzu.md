# XDNA1 host arayuzu -- dogrulanmis referans

Bu belgedeki her deger, Linux'un stock `amdxdna` surucusunun herkese acik
kaynagindan cikarilmistir (`drivers/accel/amdxdna/`). Tahmin edilen degerler
acikca isaretlidir. Kod tarafindaki karsiliklari
`include/xdna/xdna_regs.h` icindedir.

## 1. PCI kimligi

| Alan | Deger | Kaynak |
| --- | --- | --- |
| vendor | `0x1022` | `amdxdna_pci_drv.c` |
| device | `0x1502` | `amdxdna_pci_drv.c` |
| revision | `0x00` | `{ 0x1502, 0x0, &dev_npu1_info }` |
| VBNV | `RyzenAI-npu1` | `npu1_regs.c` |
| firmware yolu | `amdnpu/1502_00/npu.sbin` | `npu1_regs.c`, `aie2_pci.c` |

Surucu MSI-X sayisini `pci_msix_vec_count()` ile okur ve **hepsini** talep
eder (`pci_alloc_irq_vectors(pdev, nvec, nvec, PCI_IRQ_MSIX)`), yani MSI-X
zorunludur.

## 2. Aperture'lar ve BAR'lar

```
MPNPU_APERTURE0_BASE = 0x3000000   REG / PSP / SMU   -> BAR0
MPNPU_APERTURE1_BASE = 0x3080000   SRAM              -> BAR2
MPNPU_APERTURE2_BASE = 0x30C0000   MAILBOX           -> BAR4
```

BAR indeksleri `npu1_regs.c` icinde sabit. BAR **boyutlari** surucu
tarafindan donanimdan okunur, yani bizim secimimiz:

| BAR | Boyut (secim) | Gerekce |
| --- | --- | --- |
| 0 | 512 KiB | aperture0 -> aperture1 mesafesi |
| 2 | 256 KiB | aperture1 -> aperture2 mesafesi; ring buffer'lari tasir |
| 4 | 64 KiB | head/tail register'lari icin fazlasiyla yeterli |

BAR 0/2/4 kullanildigina gore gercek donanimda ucu de 64-bit BAR'dir
(her biri iki BAR yuvasi tuketir). Emulator de oyle yapiyor.

MSI-X tablosu icin ayri BAR kalmadigindan tabloyu BAR0'in bos ust bolgesine
koyuyoruz (`0x40000` / `0x50000`); registerlar `0x11000`'in altinda kaliyor.

## 3. Register haritasi (BAR0 offsetleri)

| Ad | Cihaz adresi | BAR0 offseti |
| --- | --- | --- |
| `MPNPU_PWAITMODE` | `0x3010034` | `0x10034` |
| `MPNPU_PUB_SEC_INTR` | `0x3010090` | `0x10090` |
| `MPNPU_PUB_PWRMGMT_INTR` | `0x3010094` | `0x10094` |
| `MPNPU_PUB_SCRATCH2..7` | `0x30100A0..B4` | `0x100A0..B4` |
| `MPNPU_PUB_SCRATCH9` | `0x30100BC` | `0x100BC` |

PSP ve SMU **ayni scratch register'larini paylasir**. Emulatorun en kolay
yanlis yapacagi yer burasi:

| Rol | Register | Not |
| --- | --- | --- |
| PSP CMD | SCRATCH2 | **STATUS ile ayni** |
| PSP STATUS | SCRATCH2 | bit31 = READY |
| PSP ARG0 | SCRATCH3 | **RESP ile ayni** |
| PSP RESP | SCRATCH3 | 0 = basarili |
| PSP ARG1 | SCRATCH4 | |
| PSP ARG2 | SCRATCH9 | `(size \| cmd<<24) & 0xFFFFFF` |
| PSP INTR | PUB_SEC_INTR | tetikleyici, `notify_val = 1` |
| PSP PWAITMODE | PWAITMODE | bit0 == 1 beklenir |
| SMU CMD | SCRATCH5 | |
| SMU ARG | SCRATCH7 | **OUT ile ayni** |
| SMU OUT | SCRATCH7 | |
| SMU RESP | SCRATCH6 | `1` = OK |
| SMU INTR | PUB_PWRMGMT_INTR | tetikleyici |

## 4. PSP protokolu (`aie_psp.c`)

```
surucu: STATUS bit31 (READY) bekle
surucu: CMD, ARG0, ARG1, ARG2 yaz
surucu: INTR <- 0, sonra INTR <- 1
cihaz : komutu isle
cihaz : RESP <- sonuc, STATUS <- READY
surucu: STATUS READY bekle, RESP oku (0 disi = hata)
```

Komutlar: `VALIDATE=1`, `START=2`, `RELEASE_TMR=3`, `VALIDATE_CERT=4`.
`START` icin `arg0 = PSP_START_COPY_FW (1)`.
Hata kodlari: `0xFFFF0002` (cancel), `0xFFFF0007` (bad state).

Boot sirasi: `VALIDATE` -> (varsa `VALIDATE_CERT`) -> `START`.
`VALIDATE` argumanlari firmware imajinin **host fiziksel adresi** ve
boyutudur; surucu tamponu `virt_to_phys()` ile veriyor, yani emulator bu
adresi guest fiziksel adresi olarak DMA ile okuyabilmelidir.

## 5. SMU protokolu (`aie_smu.c`)

```
surucu: RESP <- 0, ARG <- arg, CMD <- cmd
surucu: INTR <- 0, sonra INTR <- 1
cihaz : OUT <- cikti, RESP <- 1 (OK) veya baska (hata)
surucu: RESP != 0 olana kadar bekle, OUT oku
```

Komutlar: `POWER_ON=3`, `POWER_OFF=4`, `SET_MPNPUCLK_FREQ=5`,
`SET_HCLK_FREQ=6`, `SET_SOFT_DPMLEVEL=7`, `SET_HARD_DPMLEVEL=8`.

`aie_smu_init()` once **POWER_OFF**, sonra **POWER_ON** gonderir.

`npu1_dpm_clk_table[]` (npuclk, hclk): `{400,800} {600,1024} x4 {720,1309}
x2 {847,1600}` -> max DPM seviyesi 7.

## 6. Firmware boot el sikismasi (`aie2_get_mgmt_chann_info`)

`PSP_START` sonrasi firmware:

1. SRAM'e `struct mgmt_mbox_chann_info` (64 bayt) yazar,
2. `FW_ALIVE_OFF`'a bu yapinin **cihaz adresini** koyar.

Surucu `FW_ALIVE_OFF`'un sifir disi olmasini bekler, yapiyi okur,
`magic == 0x55504E5F` ("_NPU") dogrular, sonra `FW_ALIVE_OFF`'u temizler.

SRAM offsetleri (`npu1_regs.c` `sram_offs[]`):

| Ad | Cihaz adresi | SRAM offseti |
| --- | --- | --- |
| `MBOX_CHANN_OFF` | `0x30A0000` | `0x20000` |
| `FW_ALIVE_OFF` | `0x30BF000` | `0x3F000` |

`mgmt_mbox_chann_info` alanlari: `x2i_tail, x2i_head, x2i_buf, x2i_buf_sz,
i2x_tail, i2x_head, i2x_buf, i2x_buf_sz, magic, msi_id, prot_major,
prot_minor, rsvd[4]`. Adresler cihaz adresi olarak verilir; surucu
`AIE2_SRAM_OFF()` / `AIE2_MBOX_OFF()` ile BAR offsetine cevirir.

`prot_major/minor` `npu1_fw_feature_table[]`'a gore dogrulanir: **major 5,
minor >= 7**. 5.8 ve ustu `AIE2_NPU_COMMAND` ozelligini acar. Emulator su an
5.7 bildiriyor -- boylece `feature_mask` bos kalir ve surucu en sade
yurutme yolunu secer.

## 7. Mailbox

Cerceve (`amdxdna_mailbox.c`):

```
struct xdna_msg_header {   /* 16 bayt */
    u32 total_size;        /* payload boyutu */
    u32 sz_ver;            /* [10:0] boyut, [23:16] protokol surumu = 1 */
    u32 id;                /* [31:24] = 0x1D magic, alt bitler slot */
    u32 opcode;
};
```

Ring buffer'lar SRAM BAR'inda, head/tail register'lari MBOX BAR'inda.
Yon adlandirmasi: `x2i` = host->firmware, `i2x` = firmware->host.

Sarmalama: yazan taraf, mesaj ring sonuna sigmiyorsa tail'e `TOMBSTONE`
(`0xDEADFACE`) yazip 0'a doner. Yazma yolunda kullanilabilir alan
`rb_size - 4`'tur.

**Zorunlu yerlesim kurali** (`aie2_pci.c`):

```
interrupt_status_register = i2x_head_register + 4
```

Bu yuzden emulatorun kanal yerlesimi `x2i_head, x2i_tail, i2x_head, INTR,
i2x_tail` sirasindadir (kanal basi 32 bayt).

Interrupt akisi: cihaz cevabi yazar, INTR register'ini 1 yapar, MSI-X
tetikler. Surucu INTR'a 0 yazarak onaylar, ring'i bosaltir, cikmadan once
INTR'i tekrar okur (yaris kosulu korumasi).

### Cevap boyutu kurali -- en kritik ayrinti

`amdxdna_mailbox_helper.c` icindeki `xdna_msg_cb()`:

```c
if (unlikely(cb_arg->size != size)) {
        cb_arg->error = -EINVAL;
        goto out;
}
```

Cevap payload'i, o opcode'un `struct <name>_resp` boyutuna **birebir** esit
olmalidir. **Hata durumunda bile.** Kisa bir "sadece status" cevabi
gondermek, surucuye gercek hata kodu yerine `-EINVAL` dondurur.
`src/xdna_mert.c` icindeki `resp_size_for()` tablosu bu esleme icin var ve
`static_assert`'lerle kilitli.

Bilinen cevap boyutlari:

| Opcode | Boyut | Not |
| --- | --- | --- |
| `GET_PROTOCOL_VERSION` 0x301 | 12 | |
| `GET_FIRMWARE_VERSION` 0x108 | 20 | |
| `QUERY_AIE_VERSION` 0x0F | 8 | |
| `QUERY_AIE_TILE_INFO` 0x0E | 48 | ic yapi packed **degil** |
| `QUERY_COL_STATUS` 0x0D | 8 | |
| `GET_RUNTIME_CONFIG` 0x10B | 12 | |
| `GET_TELEMETRY` 0x04 | 16 | status **en sonda** |
| `CREATE_CONTEXT` 0x02 | 76 | 2 x cq_pair dahil |
| `REGISTER_ASYNC_EVENT_MSG` 0x10C | 8 | hemen cevaplanmaz |
| `GET_APP_HEALTH` 0x114 | 36 | |
| `GET_DEV_REVISION` 0x117 | 12 | |
| `CHAIN_EXEC_*` | 12 | `cmd_chain_resp` |
| digerleri | 4 | sadece status |

`REGISTER_ASYNC_EVENT_MSG` ozel: firmware bunu **hemen cevaplamaz**;
asenkron bir olay olustugunda cevap olarak dondurur. Emulator de oyle
davranir.

## 8. Context olusturma

`create_ctx_req` (28 bayt): `aie_type`, `start_col`, `num_col`,
`num_unused_col`, `num_cq_pairs_requested`, `pasid`, `sec_comm_target_type`,
`context_priority`.

`create_ctx_resp` (76 bayt) icinde `context_id`, `msix_id` ve
`cq_pair[2]` doner. Surucu `cq_pair[0]`'i kullanir ve context kanalinin
interrupt register'ini yine `i2x_head + 4` olarak hesaplar.

`hwctx_limit = 6` (`npu1_dev_priv`) -- Phoenix/Hawk Point alti eszamanli
workload context destekler.

## 9. Boot dizisi (`aie2_hw_start`)

```
pci_enable_device / set_master
mailbox olustur, mgmt kanali ayir
aie_smu_init()          POWER_OFF, POWER_ON
aie_psp_start()         VALIDATE [, VALIDATE_CERT], START
aie2_get_mgmt_chann_info()   FW_ALIVE bekle, info oku, magic dogrula
pci_irq_vector(msi_id) / mailbox kanalini baslat
aie2_mgmt_fw_init()     runtime cfg -> ASSIGN_MGMT_PASID -> UPDATE_PROPERTY*
                        -> SUSPEND -> RESUME
aie2_pm_init()
aie2_mgmt_fw_query()    GET_FIRMWARE_VERSION, QUERY_AIE_VERSION,
                        QUERY_AIE_TILE_INFO
aie2_error_async_events_alloc()  REGISTER_ASYNC_EVENT_MSG
```

`*` = protokol 5.7'de feature kapali oldugundan gonderilmez.

`tests/test_boot.c` bu diziyi birebir taklit eder ve emulatoru sadece MMIO
uzerinden surer.

## 10. Array topolojisi -- DOGRULANMAMIS

`QUERY_AIE_TILE_INFO` cevabindaki degerler AMD'nin acik dokumantasyonundan
ve bilinen 5 kolon x 4 compute tile topolojisinden turetilmistir; gercek
Hawk Point ciktisiyla karsilastirilmamistir:

```
Memory    M0    M1    M2    M3    M4
Compute   C00   C01   C02   C03   C04
Compute   C10   C11   C12   C13   C14
Compute   C20   C21   C22   C23   C24
Compute   C30   C31   C32   C33   C34
          5 kolon, 20 compute tile, 2560 KiB on-chip L2
```

`cols=5, rows=6, core_rows=4, mem_rows=1, shim_rows=1`,
`core_row_start=2, mem_row_start=1, shim_row_start=0`.
DMA kanali / lock / event sayilari ve `col_size` (kolon durumu dump boyutu,
su an `0x2000`) dogrulanmayi bekliyor.
