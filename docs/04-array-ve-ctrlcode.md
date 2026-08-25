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

### Opcode listesi -- eski kaynak yaniltici

`aie-rt`'nin `main-aie` dali **eski ve kisa** bir opcode listesi tasiyor:

```c
XAIE_IO_WRITE, BLOCKWRITE, BLOCKSET, MASKWRITE, MASKPOLL,
XAIE_CONFIG_SHIMDMA_BD = 5, XAIE_CONFIG_SHIMDMA_DMABUF_BD = 6, ...
```

Gercek ctrlcode bunu kullanmiyor. Derleyicinin kendi kaynak-doğruluk
basligi (`Xilinx/mlir-aie`, `include/aie/Runtime/TxnEncoding.h`) konuyu
acikca uyariyla anlatiyor:

> These DO NOT match the third_party/aie-rt xaie_txn.h enum, which is an
> older layout (CONFIG_SHIMDMA_BD=5, no NOOP/PREEMPT/LOADPDI block). They
> match the newer firmware opcodes the compiler emits.

Gecerli liste (bizim `xdna_aie.h` bununla birebir ayni):

```
WRITE 0  BLOCKWRITE 1  BLOCKSET 2  MASKWRITE 3  MASKPOLL 4
NOOP 5  PREEMPT 6  MASKPOLL_BUSY 7  LOADPDI 8  LOAD_PM_START 9
CREATE_SCRATCHPAD 10  UPDATE_STATE_TABLE 11  UPDATE_REG 12
UPDATE_SCRATCH 13  CONFIG_SHIMDMA_BD 14  CONFIG_SHIMDMA_DMABUF_BD 15
CUSTOM_OP_BEGIN 128 = TCT  DDR_PATCH 129  READ_REGS 130
RECORD_TIMER 131  MERGE_SYNC 132
```

### Tile ADRESTEN cozulur, op basligindan DEGIL

Bu, gercek workload'lari calistirmayi engelleyen bir hataydi ve
derleyicinin encoder'ina karsi differential test ile yakalandi.

`XAie_OpHdr` bir `Col` ve `Row` alani tasiyor, ama derleyici write32
uretirken o kelimeyi **rezerve birakip sifir yaziyor** ve tile'i mutlak
adrese katiyor (`TxnEncoding.h`, `txn_append_write32`: `txn[pos+1] is
reserved (0)`). `aie-rt`'nin kendi playback'i da adresi dogrudan
kullaniyor; `OpHdr`'daki alanlar relocation icin bilgi amacli.

Yorumlayici baslik alanlarini kullanirsa **derleyici ciktisindaki her
yazma tile (0,0)'a gider** -- yani hicbir gercek workload calismaz.
Emulator artik tile'i adresten cozuyor; baslik alanlari sifir disiysa ve
adresle celisirse uyari basiyor.

### Header alaninda `DevGen`

Derleyici NPU1 (Phoenix/Hawk Point) icin `devGen = 3` yaziyor
(`TxnDeviceInfo`: "3 = NPU (PHX/HWK), 4 = NPU2 (STX/KRK)"), `aie-rt`'nin
`XAIE_DEV_GEN_AIEML = 2` degerini degil. Yorumlayici bu alani yalnizca
logluyor, davranisa etkisi yok -- ama iki degerin de gorulmesi normal.

### Op boyutu: iki farkli kural

Bu, yorumlayicida gizli bir hataydi ve capraz kontrolle yakalandi:

| Op | Boyut kaynagi |
| --- | --- |
| `WRITE`, `BLOCKWRITE`, `MASKWRITE`, `MASKPOLL`, `MASKPOLL_BUSY` | baslikta `Size` alani |
| custom op'lar (`TCT`, `DDR_PATCH`, ...) | `XAie_CustomOpHdr.Size` |
| `NOOP` (4), `PREEMPT` (4), `LOADPDI` (16), `LOAD_PM_START` (8) | **SABIT**, `Size` alani YOK |

Sabit boyutlu bir op'u custom op sanip `Size` okumak coplu bir deger verir
ve ctrlcode'un geri kalanini yanlis cozer. Bu yuzden **tanimadigimiz bir
opcode'da artik hata donduruyoruz** -- boyutunu bilemedigimiz icin devam
etmek guvenli degil.

### Uygulanan opcode'lar

| Opcode | Durum |
| --- | --- |
| `XAIE_IO_WRITE` | uygulandi |
| `XAIE_IO_MASKWRITE` | uygulandi |
| `XAIE_IO_MASKPOLL` | uygulandi (ust sinirli; sinira dayanmak hata) |
| `XAIE_IO_MASKPOLL_BUSY` | MASKPOLL gibi islenıyor (varyant semantigi dogrulanmadi) |
| `XAIE_IO_BLOCKWRITE` | uygulandi |
| `XAIE_IO_BLOCKSET` | uygulandi (serilestirmede BLOCKWRITE'a donusuyor) |
| `XAIE_IO_NOOP` | uygulandi (dogru boyutla atlaniyor) |
| `XAIE_IO_CUSTOM_OP_TCT` | **uygulandi** -- asagiya bakin |
| `XAIE_IO_CUSTOM_OP_DDR_PATCH` | taniniyor, **uygulanmiyor -> HATA** |
| `CONFIG_SHIMDMA_BD`, `CONFIG_SHIMDMA_DMABUF_BD` | taniniyor, **uygulanmiyor -> HATA** |
| `PREEMPT`, `LOADPDI`, `LOAD_PM_START` | taniniyor, dogru boyutla atlaniyor |
| `READ_REGS`, `RECORD_TIMER`, `MERGE_SYNC` | teshis op'lari, atlaniyor |
| bilinmeyen opcode | **hata** (boyut bilinemez) |

BD **adresini** belirleyen op'lar (`DDR_PATCH`, `CONFIG_SHIMDMA_BD`)
atlanmiyor, **hata donduruyor**: atlanirlarsa DMA yanlis adrese gider ve
sonuc sessizce yanlis olur. Teshis op'larini atlamak sonucu
degistirmiyor, onlar uyariyla geciliyor.

### TCT (Task Completion Token)

Payload (`aiebu` `aie2p_passes.cpp`):

```
word:   bayt2 = kolon, bayt1 = satir, bayt0 = yon (1 = MM2S, 0 = S2MM)
config: bayt3 = kanal, bayt2 = kolon sayisi, bayt1 = satir sayisi
```

Bir DMA kanalinin gorevini tamamlamasini bekler. Emulatorde DMA'lar
**senkron** tamamlandigi icin token zaten hazir; yapilan is kanalin
gecerliligini dogrulamak. Bu bir tahmin degil, modelimizin dogrudan
sonucu -- DMA asenkron hale getirilirse burasi gercek beklemeye donusur.

### DDR_PATCH neden hala uygulanmiyor

**Kodlama artik tam biliniyor** -- derleyicinin kendi encoder'indan
(`TxnEncoding.h`, `txn_append_address_patch`), 12 kelime = 48 bayt:

```
w0  opcode (129)        w1  op boyutu (48)
w2..w4  rezerve         w5  action (0 = patch)
w6  yamanacak register adresi     w7  rezerve
w8  buffer argumani indeksi       w9  rezerve
w10 argplus (buffer icindeki BAYT offseti)   w11 rezerve
```

Anlam da biliniyor (`AIEDmaToNpu.cpp`): yamanacak adres bir shim BD'sinin
adres kelimesi (`getDmaBdAddress + getDmaBdAddressOffset`) ve oraya
`arg[argidx] + argplus` yazilir. Derleyici BD'yi once blok-yazip adres
alanini sifir biraktigi icin "yaz" ile "ekle" ayni sonucu verir
(`aiebu requires the block-write to precede the address patch and cover
the patched word`).

Kalan **iki** dogrulanmamis nokta var ve ikisi de yanlis olursa DMA
yanlis adrese gider:

1. **Argumanlarin komut icindeki paketlenmesi.** `exec_dpu_req.payload`
   "properties and regular kernel arguments" tutuyor, `inst_prop_cnt`
   kadar property onde geliyor. Buffer argumanlarinin 64-bit mi (iki
   kelime) yoksa 32-bit mi indeksledigi dogrulanmadi.
2. **64-bit adresin BD'ye bolunmesi.** Yamanacak adres shim BD'sinin
   `ADDRLO` kelimesi; ust bitlerin `ADDRHI`'ye nasil yerlestigi ve o
   kelimenin diger alanlarinin nasil korundugu dogrulanmadi.

Ayrica bir kip belirsizligi var: firmware, "instruction buffer" calisma
kipinde **ilk bes** host argumanina `0x80000000` ekliyor
(`AIETargetNPU.cpp`: `kDDRAIEAddrOffset`, `kNumFirmwareTranslatedArgs`);
sonrakiler icin derleyici bu offseti `argplus`'a kendisi katiyor. Tam-ELF
kipinde ise hicbirine eklenmiyor. Komuta bakarak hangi kipte oldugumuzu
ayirt edemiyoruz.

Bu yuzden op **hata donduruyor**. Uydurup uygulamak, projedeki tek
"sessizce yanlis sonuc" kaynagi olurdu.

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

## 8b. Derleyicinin kendi encoder'ina karsi dogrulama

ctrlcode yorumlayicisi artik "okudugum baslikta boyle yaziyordu"ya degil,
**derleyicinin urettigi bayt'lara** karsi dogrulaniyor.

`tools/gen-txn-vectors.cpp`, `Xilinx/mlir-aie`'nin
`include/aie/Runtime/TxnEncoding.h` basligini dogrudan `#include` edip
ctrlcode uretiyor. O baslik MLIR/LLVM'e bagimli degil ve mlir-aie'nin
kendi ifadesiyle:

> the single in-tree source of truth for the instruction format, used by
> both the compiler (AIETargetNPU.cpp) and generated host code

Yani gercek workload'lar icin kullanilan ureticinin ta kendisi.

Uretilen iki vektor (`tests/txn_vectors.h`) `tests/test_exec.c` icinde
kosuyor:

| Vektor | Ne dogruluyor |
| --- | --- |
| `txn_vec_loopback` | 26 op'luk tam veri yolu: host -> shim -> memory tile -> shim -> host. Cikis girisle birebir ayni olmali. |
| `txn_vec_blockwrite_out` | Tile'in ADRESTEN cozuldugunu **veriyle** olcer: desen memory tile (0,1) bellegine blok-yazilip ayni yerden DMA ile cikarilir. |

Ikinci vektor bir regresyon testi: tile cozumu op basligina geri
dondurulunce iki kontrol de dusuyor (olculdu).

Yeniden uretim:

```sh
g++ -std=c++17 -I<mlir-aie>/include -Iinclude \
    -o gen-txn-vectors tools/gen-txn-vectors.cpp
./gen-txn-vectors > tests/txn_vectors.h
```

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

## 9a. Cihaz bellegi (AIE2_DEVM)

Cihaz bellegi ayri bir fiziksel bellek degil; context'in host heap
tamponuna bakan bir **adres penceresi**:

```
cihaz adresi D  ->  host adresi = heap_addr + (D - AIE2_DEVM_BASE)
```

`AIE2_DEVM_BASE = 0x4000000`, pencere 64 MiB. Heap'in host adresini
firmware `MAP_HOST_BUFFER` ile ogreniyor.

Kaynak: `amdxdna_gem.c` -- heap BO'nun cihaz adresi `dev_mem_base`'den
basliyor (`abo->dev_addr = dev_info->dev_mem_base + total_heap_size`) ve
devm penceresindeki offset `dev_addr - dev_mem_base`.

`SYNC_BO` bu ceviriyi yapiyor: `type` alaninin alt nibble'i kaynak, ust
nibble'i hedef turu (`SYNC_BO_DEV_MEM = 0`, `SYNC_BO_HOST_MEM = 2`).
Heap siniri disina tasan cihaz adresleri reddediliyor.

## 9b. Asenkron hata bildirimi

Array'de bir hata olustugunda emulator bunu surucuye gercek protokolle
bildiriyor (`src/xdna_error.c`):

```
surucu: REGISTER_ASYNC_EVENT_MSG (tampon adresi)   -> CEVAP YOK, bekletilir
   ...
array : DMA / lock / stream hatasi
firmware: tampona aie_err_info + aie_error yaz
firmware: bekleyen mesaja cevap don (status, type = AIE_ERROR)
surucu: hatayi siniflandir, olayi YENIDEN kaydet
```

Olay kimlikleri `aie2_error.c` LUT'larindan alindi; uydurma bir `event_id`
surucu tarafinda "unknown" olarak siniflanirdi:

| Tile | DMA | LOCK | STREAM | mod_type |
| --- | --- | --- | --- | --- |
| shim | 72 | 74 | 65 | `AIE_PL_MOD` (2) |
| memory (satir 1) | 133 | 139 | 135 | `AIE_MEM_MOD` (0) |
| compute | 97 | 101 | 56 | `AIE_MEM_MOD` / stream icin `AIE_CORE_MOD` |

Surucu `AIE_MEM_MOD` icin LUT'u satira gore seciyor (`row == 1` -> memory
tile), bu da topolojimizle birebir ortusuyor.

Hata kaydedilmemisse (surucu henuz olay kaydetmemis) sessizce yutulmuyor,
uyari loglaniyor.

## 10. Bu katmanda hala eksik olan

- **AIE instruction interpreter** (asama 7). `CORE_CONTROL` yazmasi kabul
  ediliyor ve `core_status` guncelleniyor, ama compute tile programi
  yurutulmuyor -- her seferinde UYARI loglaniyor.
- **Stream switch yonlendirmesi** (yukarida).
- **Overlay / PDI yuklemesi**: `CONFIG_CU` CU eslemesini kaydediyor ama PDI
  imajini array'e yuklemiyor.
- **Custom op'lar**: TCT ve DDR_PATCH.
- **Paket anahtarlamali stream** ve trace portlari.
- **BD adreslerinde cihaz adresi cevirisi**: `SYNC_BO` cevriyor, ama shim
  BD'lerine yazilan adresler ham kabul ediliyor (gercek ctrlcode bunlari
  DDR_PATCH ile yamiyor).
