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
(`req.pasid = amdxdna_pasid_on(client) ? client->pasid : 0`).

> **Sonradan duzeltme:** carveout'un npu1 yolunda kurulamayacagini
> dusunmustuk. Yanlismis: `amdxdna_debugfs.c` carveout'u **debugfs'ten**
> ayarlayan bir dosya sunuyor, aygittan bagimsiz. Asagidaki 5. bulguya
> bakin -- `/dev/accel/accel0` bu yolla aciliyor.

### DENEY SONUCLARI (gercek guest, QEMU master + Linux master)

Bu bolum artik spekulasyon degil: emulator gercek bir guest'te stock
`amdxdna` ile kosturuldu (`qemu/guest-test/`). Bulgular:

**1. Probe geciyor, `/dev/accel/accel0` olusuyor.**

```
amdxdna 0000:00:03.0: [drm] Load firmware amdnpu/1502_00/npu.sbin
[drm] Initialized amdxdna_accel_driver 0.10.0 for 0000:00:03.0 on minor 0
/sys/class/accel/accel0/device/vbnv = RyzenAI-npu1
/sys/class/accel/accel0/device/fw_version = 5.7.0.0
```

Yani SMU guc dizisi, PSP firmware yuklemesi, firmware el sikismasi,
mailbox ve surum sorgulari **gercek surucuyle** calisiyor.

**2. `open()` SVA'da takiliyor.**

```
amdxdna_sva_init: SVA bind device failed, ret -19
amdxdna_drm_open: PASID not available for pid 1
amdxdna_drm_open: PASID unavailable and carveout not configured
```

**3. `force_iova=1` CALISMIYOR -- caveat dogrulandi.**

```
amdxdna_iommu_init: Enabled force_iova mode.
amdxdna_iommu_init: Failed to alloc iommu domain
probe with driver amdxdna failed with error -95
```

Domain `IOMMU_HWPT_ALLOC_PASID` bayragiyla ayriliyor; vIOMMU bunu
desteklemeyince probe TAMAMEN basarisiz oluyor. Yani IOVA modu, SVA
olmayan ortamda kurtarici degil -- durumu daha da kotulestiriyor.

**4. QEMU'nun amd-iommu'sunda PASID destegi YOK.**

`hw/i386/amd_iommu.c` icinde "pasid" gecen tek satir yok. Buna karsilik
`hw/i386/intel_iommu.c` icinde 367 gecis var (scalable mode, `svm=on`,
`pasid-bits`). Yani bugun VM icinde PASID'e giden tek yol Intel vIOMMU.

**5. Intel vIOMMU + SVM ile daha ileri gidiliyor ama SVA yine baglanmiyor.**

`-device intel-iommu,scalable-mode=on,svm=on,fsts=on,pasid-bits=16,`
`device-iotlb=on,intremap=on` ile hata `-28` yerine `-19` oluyor.
Aygitimiz ATS + PRI + PASID genisletilmis yeteneklerini bildiriyor ve
guest bunlari goruyor:

```
ext cap @0x100 id=0x000f (ATS)
ext cap @0x140 id=0x0013 (PRI)
ext cap @0x180 id=0x001b (PASID)
```

Kalan engel emulatorde degil, vIOMMU/kernel SVA etkinlestirme yolunda.

**6. Yan bulgu: PSP yolu IOMMU'dan gecmiyor.**

Ilk denemede IOMMU ceviri modundayken firmware yuklemesi sayfa hatasi
verdi:

```
DMAR: [DMA Read NO_PASID] Request device [0000:00:03.0] fault addr 0x2410000
      [fault reason 0x71] SM: Present bit in first-level paging entry is clear
```

Sebep: surucu firmware tamponunun adresini `virt_to_phys()` ile veriyor,
yani DMA API'sini atliyor. Gercek donanimda PSP fiziksel bellege dogrudan
erisiyor. Emulator artik bunu ayri bir `phys_read` callback'i ile
modelliyor (QEMU tarafinda `address_space_read`), ve ceviri acikken de
firmware yukleniyor. **Bu, emulatorun fidelity'sini artiran gercek bir
duzeltmeydi.**

**7. Yan bulgu: surucu hipervizor altinda calismayi reddediyor.**

`aie2_pci.c`:

```c
if (!hypervisor_is_type(X86_HYPER_NATIVE)) {
        XDNA_ERR(xdna, "Running under hypervisor not supported");
        return -EINVAL;
}
```

QEMU'yu **TCG ile** (yani `-accel kvm` olmadan) kosturunca kernel
`X86_HYPER_NATIVE` goruyor ve kontrol geciyor. **KVM ile bu kontrol
basarisiz olur** -- performansli bir kurulum icin bu ayri bir engel.

**5. SVA gerekmiyormus: surucunun kendi carveout yolu var.**

`amdxdna_drm_open`, PASID alinamadiginda **carveout** bellek
yapilandirilmissa `open()`'i basarili sayiyor:

```c
if (amdxdna_sva_init(client)) {
        XDNA_WARN(xdna, "PASID not available for pid %d", client->pid);
        if (!amdxdna_use_carveout(xdna)) {
                XDNA_ERR(xdna, "PASID unavailable and carveout not configured");
                ret = -EINVAL;
```

Carveout, surucunun **kendi debugfs arayuzunden** ayarlanan fiziksel
olarak surekli bir bellek blogu (`amdxdna_debugfs.c`):

```
/sys/kernel/debug/accel/<pci-adresi>/carveout  <-  "<boyut>@<adres>"
```

Guest tarafinda hicbir sey degistirilmiyor -- bu stock surucunun kendi
ozelligi ("platform debug/bringup feature"). Tek gereken, o fiziksel
bolgenin kernel tarafindan kullanilmiyor olmasi (`memmap=128M$0x...`)
ve `CONFIG_DEBUG_FS=y`.

Bununla `/dev/accel/accel0` aciliyor, BO'lar ayriliyor, context
olusuyor ve **uctan uca workload kosuyor**.

**6. QEMU'nun amd-iommu'su aygit DMA'sini CEVIRMIYOR.**

Bu, uctan uca yolda ortaya cikti: BO'lar IOVA ile adresleniyor
(orn. heap IOVA'si `0xbc000000`), 2 GB RAM'li bir guest'te bu adres
RAM'in disinda. `-device amd-iommu` ile emulatorun DMA okumasi hep
`0xff` donuyor; `-device intel-iommu,...` ile ceviri dogru yapiliyor ve
her sey calisiyor. Bu yuzden `run.sh` varsayilani **intel** oldu.

### Sonuc

Sirali plan guncellendi:

1. **Bugun calisan yapilandirma:** `-device intel-iommu,scalable-mode=on,...`
   + TCG + carveout. Probe geciyor, `/dev/accel/accel0` **aciliyor**,
   sorgular calisiyor ve **uctan uca workload dogru cikti uretiyor**.
   `-device amd-iommu` ile probe geciyor ama DMA cevrilmedigi icin
   workload calismiyor.
2. **SVA'nin kendisi hala baglanmiyor.** Aygit tarafinda yapilabilecek her
   sey yapildi (ATS/PRI/PASID bildiriliyor); kalan is QEMU/kernel
   tarafinda. Carveout bunu gerektirmeyen bir yol acti, ama process
   basina ayri PASID izolasyonu icin SVA yine de dogru cozum.
3. **KVM istenirse** hipervizor tespiti ayrica ele alinmali.

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
