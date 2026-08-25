# XDNA array modeli ve ctrlcode yurutmesi

Bu belge asama 5, 6 ve 8'in nasil uygulandigini anlatiyor. Sabitlerin
tamami AMD/Xilinx `aie-rt` surucusunun acik kaynagindan dogrulandi
(`include/xdna/xdna_aie.h` icinde her blogun kaynagi yazili).

## 1. Topoloji -- artik tahmin degil

`aie-rt/driver/tests/stest/hw_config.h` icindeki `AIE_GEN 2` / `DEVICE 0`
blogu Phoenix yerlesimiyle birebir ayni:

```
satir 5   C40  C41  C42  C43  C44     compute
satir 4   C30  C31  C32  C33  C34     compute
satir 3   C20  C21  C22  C23  C24     compute
satir 2   C10  C11  C12  C13  C14     compute
satir 1   M0   M1   M2   M3   M4      memory tile (512 KiB)
satir 0   S0   S1   S2   S3   S4      shim / NOC

5 kolon x 4 compute satiri = 20 compute tile
5 x 512 KiB = 2560 KiB on-chip L2
```

Lock ve DMA kanal sayilari da ayni kaynaktan geliyor: compute tile 16 lock /
2 DMA kanali, memory tile 64 lock / 6 kanal, shim 16 lock / 2 kanal.

## 2. Adres kodlamasi

```
adres = (kolon << 25) | (satir << 20) | tile_ici_offset
```

`AIE_COL_SHIFT 25`, `AIE_ROW_SHIFT 20` -- `xaie_lite_hwcfg.h` (AIE_GEN 2).

## 3. Tile modeli

Her tile'in durumu `AieTile` icinde:

| Alan | Compute | Memory | Shim |
| --- | --- | --- | --- |
| veri bellegi | 64 KiB @ 0x00000 | 512 KiB @ 0x00000 | yok |
| program bellegi | 16 KiB @ 0x20000 | yok | yok |
| lock | 16 @ 0x40000 | 64 @ 0xD0000 | 16 @ 0x40000 |
| lock deger reg | 0x1F000 | 0xC0000 | 0x14000 |
| BD | 16 @ 0x1D000 | 48 @ 0xA0000 | 16 @ 0x1D000 |
| DMA S2MM/MM2S | 0x1DE00 / 0x1DE10 | 0xA0600 / 0xA0630 | 0x1D200 / 0x1D210 |

**BD sayisi tile turune gore farkli**: shim ve compute tile'da BD id alani
4 bit (16 BD), memory tile'da 6 bit (48 BD). Tek bir sabit kullanmak
shim'de BD bolgesinin DMA kontrol registerlarinin (0x1D200) uzerine
binmesine yol aciyor -- bu hata gelistirme sirasinda yakalandi.

Modellenmeyen registerlar kaybolmuyor: tile basina seyrek bir tabloda
saklanip aynen geri okunuyor. Boylece ctrlcode'un yaptigi her yazma
tutarli kaliyor ve tanimadigimiz bir registera yapilan MASKPOLL beklendigi
gibi calisiyor.

## 4. Lock semantigi

AIE-ML lock'lari **semafor**dur, ikili kilit degil:

- `release(v)`: lock += v
- `acquire_ge(v)`: lock >= v ise lock -= v ve basari, degilse basarisiz

Islem, LOCK_REQUEST bolgesine yapilan bir **OKUMA** ile tetiklenir
(`aie-rt` bunu `XAie_MaskPoll` ile yapar) ve donen degerin bit0'i sonuctur:

```
offset = LOCK_REQUEST + id * 0x400 + (acquire ? 0x200 : 0) + ((v & 0x7F) << 2)
```

`v` 7 bit isaretli (-64..63).

## 5. Buffer descriptor'lar

Uc tile turunun BD yerlesimi **farklidir**; ortak bir format varsaymak
yanlis olur. Hepsi ayri ayri dogrulandi:

| Alan | Shim | Memory tile | Compute tile |
| --- | --- | --- | --- |
| uzunluk | w0[31:0] | w0[16:0] | w0[13:0] |
| adres | w1[31:2] + w2[15:0] (64-bit host) | w1[18:0] (kelime) | w0[27:14] (kelime) |
| valid | w7[25] | w7[31] | w5[25] |
| next BD | w7[30:27], use w7[26] | w1[25:20], use w1[19] | w5[30:27], use w5[26] |
| lock acquire | w7[12], [11:5], [3:0] | w7[15], [14:8], [7:0] | w5[12], [11:5], [3:0] |
| lock release | w7[24:18], [16:13] | w7[30:24], [23:16] | w5[24:18], [16:13] |

Uzunluklar 32-bit **kelime** cinsindendir.

## 6. DMA

Bir kanalin "task queue" / "start queue" registerina yazmak gorevi
baslatir. Emulator BD zincirini yurur:

```
acquire lock (varsa) -> veri transferi -> release lock (varsa) -> next BD
```

Shim tile icin adres host fiziksel adresidir (DMA callback'i uzerinden);
memory ve compute tile icin tile-yerel bellek offsetidir.

## 6b. Stream switch

Tile'lar arasi veri, stream switch uzerinden akar. Devre anahtarlamali
(circuit-switched) yonlendirme modellendi:

```
MASTER_CONFIG[m].CONFIGURATION = bu master portu besleyen slave port
MASTER_CONFIG[m].MASTER_ENABLE = baglanti etkin mi
SLAVE_CONFIG[s].SLAVE_ENABLE   = slave port etkin mi
```

Register tabanlari: compute tile master 0x3F000 / slave 0x3F100, memory tile
0xB0000 / 0xB0100, shim 0x3F000 / 0x3F100. Port indeksi = register
siralamasi.

### Port tablolari

| Tile | Master portlari | Slave portlari |
| --- | --- | --- |
| compute | CORE0, DMA0-1, TILE_CTRL, FIFO0, SOUTH0-3, WEST0-3, NORTH0-5, EAST0-3 | CORE0, DMA_0-1, TILE_CTRL, FIFO_0, SOUTH_0-5, WEST_0-3, NORTH_0-3, EAST_0-3, AIE_TRACE, MEM_TRACE |
| memory | DMA0-5, TILE_CTRL, SOUTH0-3, NORTH0-5 | DMA_0-5, TILE_CTRL, SOUTH_0-5, NORTH_0-3, TRACE |
| shim | TILE_CTRL, FIFO0, SOUTH0-5, WEST0-3, NORTH0-5, EAST0-3 | TILE_CTRL, FIFO_0, SOUTH_0-7, WEST_0-3, NORTH_0-3, EAST_0-3, TRACE |

### Komsu baglantisi

```
master NORTH<k>  ->  ustteki tile'in slave SOUTH_<k>
master SOUTH<k>  ->  alttaki tile'in slave NORTH_<k>
master EAST<k>   ->  sagdaki tile'in slave WEST_<k>
master WEST<k>   ->  soldaki tile'in slave EAST_<k>
```

Port sayilari bu eslemeyi dogruluyor: compute tile'in 6 NORTH master portu
var, ustundeki tile'in 6 SOUTH slave portu; 4 SOUTH master, alttakinin
4 NORTH slave portu. Shim'in 6 NORTH master'i memory tile'in 6 SOUTH
slave'ine, memory tile'in 4 SOUTH master'i shim'in 4 NORTH slave'ine
denk geliyor.

### Shim DMA baglantisi

Shim'in kendi stream switch'inde DMA portu yok; DMA guney portlarindan
gecer ve MUX/DEMUX registerlari bu portlarin NoC/DMA/PL'den hangisine
bagli oldugunu secer (`xaie_plif.c`):

| Yon | Port | Register |
| --- | --- | --- |
| host -> AIE (MM2S) | slave SOUTH 3 veya 7 | MUX_CONFIG (0x1F000), alanlar SOUTH2@8, SOUTH3@10, SOUTH6@12, SOUTH7@14 |
| AIE -> host (S2MM) | master SOUTH 2 veya 3 | DEMUX_CONFIG (0x1F004), alanlar SOUTH2@4, SOUTH3@6, SOUTH4@8, SOUTH5@10 |

Alan degerleri: 0 = PL, 1 = DMA, 2 = NOC.

**Cikarim (dogrulanmadi):** kanal <-> port eslemesi sirayla varsayildi
(MM2S ch0 -> SOUTH_3, ch1 -> SOUTH_7; S2MM ch0 -> SOUTH2, ch1 -> SOUTH3).
Gercek donanimla karsilastirilmali.

### Kalan

Paket anahtarlamali (packet-switched) akis, slot/arbitrasyon registerlari ve
trace portlari modellenmedi. Compute tile'in CORE stream portlari da yok --
AIE interpreter olmadigi icin veri gidecek yer yok; bu porta ulasan veri
sessizce yutulmuyor, uyari veriliyor.

## 7. ctrlcode bicimi

```
XAie_TxnHeader (16 bayt)
  Major, Minor, DevGen, NumRows, NumCols, NumMemTileRows, NumOps, TxnSize
[ op kaydi ] [ op kaydi ] ...
```

Her op kaydi kendi `Size` alanini tasir (baslik + payload, bayt), yani
yurutucu tanimadigi bir opcode'u bile dogru atlayabilir.

Yapilar `aie-rt`'de **packed degildir**; dogal hizalama gecerlidir.
Boyutlar `static_assert` ile kilitli:

| Yapi | Boyut |
| --- | --- |
| `XAie_TxnHeader` | 16 |
| `XAie_OpHdr` | 3 |
| `XAie_Write32Hdr` | 24 |
| `XAie_MaskWrite32Hdr` / `MaskPoll32Hdr` | 32 |
| `XAie_BlockWrite32Hdr` | 16 |
| `XAie_CustomOpHdr` | 8 |

### Uygulanan opcode'lar

| Opcode | Durum |
| --- | --- |
| `XAIE_IO_WRITE` | uygulandi |
| `XAIE_IO_MASKWRITE` | uygulandi |
| `XAIE_IO_MASKPOLL` | uygulandi (ust sinirli; sinira dayanmak hata) |
| `XAIE_IO_BLOCKWRITE` | uygulandi |
| `XAIE_IO_BLOCKSET` | uygulandi (serilestirmede BLOCKWRITE'a donusuyor) |
| `XAIE_IO_CUSTOM_OP_TCT` | **atlaniyor**, uyari veriliyor |
| `XAIE_IO_CUSTOM_OP_DDR_PATCH` | **atlaniyor**, uyari veriliyor |
| `XAIE_CONFIG_SHIMDMA_BD` / `_DMABUF_BD` | **atlaniyor**, uyari veriliyor |

Custom op'larin payload yerlesimi `aie-rt` icinde tanimli degil (XRT
tarafinda); dogrulanmadan uygulanmalari yanlis sonuc uretir. Atlanan her op
loglaniyor ve kosum sonunda "sonuc eksik olabilir" uyarisi veriliyor --
sessiz basarisizlik yok.

### Partition siniri

ctrlcode kolon numaralari partition'a gorecelidir; emulator
`col < ctx->num_col` kontrolunu zorlar. Bunu zorlamamak context'ler arasi
izolasyonu modellenmemis birakirdi.

## 8. Yurutme yolu (mesajdan array'e)

```
XRT -> amdxdna -> hwctx mailbox kanali
   MSG_OP_EXEC_DPU { inst_buf_addr, inst_size }
        |
        v
   MERT modeli: instruction buffer'i host bellegindin DMA ile oku
        |
        v
   ctrlcode yurutucusu (xdna_txn.c)
        |
        v
   XDNA array (xdna_array.c): BD'ler, lock'lar, DMA, tile bellegi
        |
        v
   host cikis tamponu + MSI-X
```

`MSG_OP_CHAIN_EXEC_DPU` ayni yolu `cmd_chain_slot_dpu` dizisi uzerinde
tekrarlar ve ilk hatada `fail_cmd_idx` ile birlikte doner.

## 9. Test

`tests/test_exec.c` gercek bicimde bir ctrlcode uretip stock surucu yolundan
gonderiyor ve su yolu dogruluyor:

```
host giris -> shim MM2S -> kolon stream'i -> memory tile S2MM (lock release)
           -> MASKPOLL(lock == 1) -> memory tile MM2S (lock acquire)
           -> kolon stream'i -> shim S2MM -> host cikis
```

Cikisin girisle birebir ayni olmasi; BD cozumleme, lock semantigi, DMA
motoru ve ctrlcode yorumlayicisinin birlikte dogru calistigini gosteriyor.
Ayrica test ediliyor: BLOCKWRITE ile yazip DMA ile geri okuma, iki kosum
arasinda lock durumunun sifirlanmasi, CHAIN_EXEC_DPU, SYNC_BO ve dort hata
yolu (saglanmayan MASKPOLL, partition disi kolon, bozuk op boyutu, bilinmeyen
opcode).

## 10. Bu katmanda hala eksik olan

- **AIE instruction interpreter** (asama 7). `CORE_CONTROL` yazmasi kabul
  ediliyor ve `core_status` guncelleniyor, ama compute tile programi
  yurutulmuyor -- her seferinde UYARI loglaniyor.
- **Stream switch yonlendirmesi** (yukarida).
- **Overlay / PDI yuklemesi**: `CONFIG_CU` CU eslemesini kaydediyor ama PDI
  imajini array'e yuklemiyor.
- **Custom op'lar**: TCT ve DDR_PATCH.
- **Cihaz bellegi (AIE2_DEVM)**: `SYNC_BO` yalnizca host-host yolunu
  destekliyor.
