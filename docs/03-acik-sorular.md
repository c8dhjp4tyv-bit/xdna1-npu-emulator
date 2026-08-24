# Acik sorular ve risk degerlendirmesi

Planin sonunda "en kritik iki arastirma konusu" olarak isaretlenen iki
problem. Ikisi de arastirildi; **ikisi de basta dusunulenden daha iyi
durumda**, ama ikisi de kapanmis degil.

---

## 1. AIE instruction-level yurutme modeli ne kadar acik?

### Bulgu: ISA dokumante degil, ama acik kaynakta **implemente**

AMD/Xilinx, AI Engine islemcileri icin bir LLVM arka ucunu acik kaynak
yapti: **Peano** / `llvm-aie`. Depo, ozellikle **Phoenix ve Hawk Point'teki
XDNA hizlandiricilarinin uyguladigi AIE2 mimarisine** odakleniyor. Ustune
`mlir-aie`, tile konfigurasyonu, stream switch'ler, lock'lar ve DMA'lar icin
MLIR seviyesinde bir arac zinciri sunuyor.

Bu, asama 7 icin oyunu degistiriyor:

| Ihtiyac | Nerede |
| --- | --- |
| Instruction encoding'leri | `llvm-aie` TableGen (`.td`) tanimlari |
| Register dosyasi, operand siniflari | `llvm-aie` TableGen |
| Pipeline / latency / kaynak catismalari | `llvm-aie` scheduling modeli |
| Intrinsic semantigi | `llvm-aie` intrinsic tanimlari + testler |
| Tile / stream / lock / DMA konfigurasyonu | `mlir-aie` |
| Assembler + disassembler | `llvm-aie` (dogrulama icin altin standart) |

Yani "instruction seti kapali, tersine muhendislik gerekir" varsayimi artik
gecerli degil. Emulatorun decoder'i TableGen tanimlarindan **uretilebilir**.

### Kalan risk

TableGen bize **encoding** ve **zamanlama** veriyor; **semantik** her zaman
dogrudan vermiyor. Vektor birimlerinin tam davranisi (saturasyon kurallari,
yuvarlama modlari, akumulator genisligi, permute agi) buyuk olcude
intrinsic tanimlarindan ve derleyici testlerinden cikarilmali. Bu mekanik
ama uzun bir is.

### Onerilen yaklasim

1. `llvm-aie`'den decoder tablosunu uret; her instruction icin bir
   "unimplemented" tuzagi birak.
2. Gercek bir workload'u kosturup hangi instruction'larin gercekten
   kullanildigini olc. Kapsam muhtemelen ISA'nin kucuk bir alt kumesi.
3. Sadece o alt kumeyi implemente et; differential test icin `llvm-aie`
   assembler'ini ve mumkunse gercek donanimi kullan.

Kaynaklar:
[Peano duyurusu (LLVM Discourse)](https://discourse.llvm.org/t/peano-llvm-support-for-amd-xilinx-ai-engine-processors/79458),
[Xilinx/llvm-aie](https://github.com/Xilinx/llvm-aie/blob/aie-public/README.md),
[Xilinx/mlir-aie](https://github.com/Xilinx/mlir-aie/tree/main),
[Hello XDNA -- ISA notlari](https://tnzr.org/xdna/isa.html),
[Phoronix](https://www.phoronix.com/news/AMD-Peano-LLVM-Ryzen-AI).

---

## 2. VM icinde stock `amdxdna` icin SVA/PASID yolu

### Problem

Surucu, bir userspace client acildiginda dogrudan
`iommu_sva_bind_device()` cagirip PASID aliyor
(`amdxdna_pci_drv.c: amdxdna_sva_init`). Her process kendi PASID'i ile ayri
bir NPU context'ine baglaniyor. VM tarafinda bunun calismasi icin
`guest amdxdna -> guest SVA -> sanal AMD IOMMU -> QEMU` zincirinin tamami
gerekiyor.

QEMU'nun `amd-iommu` aygiti DMA remapping ve interrupt remapping'i
belgeliyor; PASID/SVA icin ayni seviyede belgelenmis bir secenek gorunmuyor.
Intel VT-d tarafinda in-guest SVM icin calisma var ama bir kismi upstream
disi.

### Bulgu: stock surucunun **iki cikis kapisi** var

Guncel upstream surucuyu okuyunca iki mekanizma cikiyor -- ikisi de
surucuyu **degistirmeden** kullanilabiliyor:

**a) `force_iova` modul parametresi** (`amdxdna_iommu.c`)

```c
static bool force_iova;
module_param(force_iova, bool, 0600);
```

`force_iova=1` iken surucu kendi IOMMU paging domain'ini ve IOVA
allocator'unu kuruyor, BO'lari `iommu_map_sgtable()` ile esliyor. `drm_open`
yolunda ise:

```c
if (!amdxdna_iova_on(xdna)) {
        ... amdxdna_sva_init(client) ...
}
```

Yani **IOVA modunda `iommu_sva_bind_device()` hic cagrilmiyor**. Bu, VM
icinde siradan IOMMU DMA eslemesine dusuyor.

*Caveat:* domain `IOMMU_HWPT_ALLOC_PASID` bayragiyla ayriliyor, dolayisiyla
PASID yetenegi olmayan bir vIOMMU'da bu cagri yine de basarisiz olabilir.
Deneyle dogrulanmali.

**b) SVA basarisizligi olumcul degil**

```c
if (amdxdna_sva_init(client)) {
        XDNA_WARN(xdna, "PASID not available for pid %d", client->pid);
        if (!amdxdna_use_carveout(xdna)) { ... return -EINVAL; }
}
```

PASID alinamazsa surucu uyarip devam ediyor -- **carveout bellek
yapilandirilmissa**. `create_ctx_req.pasid` o durumda 0 gonderiliyor
(`req.pasid = amdxdna_pasid_on(client) ? client->pasid : 0`). Elimizdeki
kaynaklarda carveout'u npu1 yolu icin kuran bir cagri yok (aie4/SR-IOV
tarafinda gorunuyor), dolayisiyla pratikte (a) daha gercekci.

### Sonuc

Asama 3 artik "QEMU AMD vIOMMU'da SVA gelistirmesi gerekebilir, yoksa proje
tikanir" degil. Sirali plan:

1. **Once IOVA modu.** `amdxdna.force_iova=1` ile guest'te probe ve context
   olusturmayi calistir. Emulator zaten PASID'i sadece kaydediyor, DMA'yi
   guest fiziksel adresi uzerinden yapiyor -- IOVA modunda bu dogru davranis.
2. **Sonra tam SVA.** Coklu process izolasyonu ve gercek per-process adres
   uzayi icin vIOMMU tarafinda PASID/PRI/ATS gerekiyor. Bu, projenin degil
   QEMU'nun isi; paralel olarak takip edilmeli.

Kaynaklar:
[QEMU VT-d ATS serisi](https://patchew.org/QEMU/20240521130946.117849-1-clement.mathieu--drif@eviden.com/),
[in-guest SVM demosu (Intel)](https://github.com/BullSequana/Qemu-in-guest-SVM-demo),
[Linux SVA belgesi](https://docs.kernel.org/arch/x86/sva.html),
[AMD IOMMU SVA (Phoronix)](https://www.phoronix.com/news/AMD-IOMMU-SVA-Nears).

---

## 3. Firmware: semantik emulasyon karari

Iki secenek vardi:

1. NPU mikrodenetleyicisini instruction seviyesinde emule etmek ve gercek
   `npu.sbin`'i kosturmak. Ilgili mikrodenetleyici mimarisi ve tum ortam
   gerekir; ayrica proprietary firmware ayrintilari sorun cikarabilir.
2. Firmware'i **semantik** olarak emule etmek: PSP imaji kabul eder,
   firmware surumu ve "alive" durumu uretir, mailbox komutlarinin
   davranisini emulator implemente eder.

**Secim: 2.** Guest acisindan sonuc ayni: stock firmware dosyasi, stock
surucu, normal boot protokolu. Firmware CPU uzerinde native calismiyor ama
gozlemlenebilir davranis birebir. Bu hala gercek bir fonksiyonel donanim
emulatoru sayilir.

`src/xdna_psp.c` imaji gercekten DMA ile okuyor (dolayisiyla yanlis adres
veya erisilemez tampon hata donduruyor), sonra `src/xdna_fw.c` MERT davranis
modelini ayaga kaldiriyor.

---

## 4. Hala dogrulanmamis olanlar

Asagidakiler icin gercek bir Hawk Point makinesinde olcum gerekiyor:

- `QUERY_AIE_TILE_INFO` alanlarinin gercek degerleri (DMA kanali, lock,
  event sayilari, `col_size`).
- `QUERY_COL_STATUS` dump formati.
- BAR boyutlari (fonksiyonel olarak onemsiz, ama `lspci` ciktisi farkli).
- `GET_TELEMETRY` icerigi.
- Hata ve reset yollarinin tam davranisi.
- Firmware surum/protokol kombinasyonlarinin XRT tarafindaki etkileri.

Bunlarin hicbiri boot'u engellemiyor; hepsi **uyumluluk cilasi**.
Karsilastirma metodu asama 9'daki yan yana kosum.
