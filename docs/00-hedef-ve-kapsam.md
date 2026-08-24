# Hedef ve kapsam

## Tek cumlede

Host makinede **fiziksel AMD NPU olmadan**, guest VM icinde **degistirilmemis
stock `amdxdna` surucusu ve stock XRT** ile gercek XDNA1 workload'larini
calistirabilen bir **fonksiyonel donanim emulatoru** yazmak.

Teknik adi: *AMD XDNA1 full-system functional emulator*. Bu bir vGPU degil;
QEMU'nun baska bir CPU mimarisini emule etmesine benziyor.

## Ne yapiyoruz

```
                       GUEST VM
  ML uygulamasi -> XRT -> stock amdxdna.ko -> /dev/accel/accel0
                                                    |
                          PCI / MMIO / DMA / MSI-X / PASID
                                                    |
                                                    v
                          QEMU: sanal AMD XDNA1 NPU aygiti
                                                    |
                                                    v
                          libxdna: PSP, SMU, MERT/ERT, mailbox,
                                   context'ler, DMA, XDNA array
                                                    |
                                                    v
                                            Host CPU'da yurutme
```

Host CPU, NPU'nun davranisini hesaplar. GPU gerekmiyor.

## Guest'te KULLANMAYACAKLARIMIZ

- sahte XRT
- sahte `amdxdna`
- `LD_PRELOAD` / API shim
- ONNX sarmalayici

Guest, gercek Hawk Point'te calisan stock yazilim yiginini kullanir.

## Basari kriteri

`xrt-smi`'nin cihazi gormesi **basari kriteri degil**, sadece ara bir
gostergedir. Gercek donanimda bile cihaz enumerate olup sonrasinda komut
gonderimi basarisiz olabiliyor.

Gercek kriter:

> Gercek Hawk Point icin derlenmis, **degistirilmemis** bir XDNA binary'si
> emulator uzerinde calisip **ayni dogru sonucu** uretir.

## Kapsam disi

| Davranis | Hedef |
| --- | --- |
| stock amdxdna, stock XRT, `/dev/accel` | evet |
| HW context'ler, DMA, PASID, interrupt, hata/reset | evet |
| gercek XDNA binary'leri, dogru cikti | evet |
| coklu workload | evet |
| gercek silikon guc tuketimi | hayir |
| gercek sicaklik | hayir |
| nanosaniye hassasiyetinde timing | gerekli degil |

Son ucu icin cycle-accurate / mikromimari simulator gerekir; bu projenin
amaci fonksiyonel uyumluluk.

## Neden dogrudan "ONNX -> CPU" olmaz

"Bu binary ResNet calistiriyor, ben CPU'da ResNet kosayim" yaklasimi ciktiyi
dogru uretse bile donanimi emule etmez. Program tile bellegi, lock'lar,
stream'ler, DMA, register'lar ve senkronizasyon gibi NPU ozelliklerine
bagimli olabilir. Sartimiz: **bilmedigimiz, desteklenen herhangi bir XDNA
workload'u** calisabilmeli. Bu da binary'nin gercek semantiginin
uygulanmasini gerektiriyor.

## Neden AMD'nin `virtio-npu` prototipi cozum degil

`-device virtio-accel-pci`, guest'e **host'taki gercek NPU'ya** sanal erisim
verir:

```
virtio-npu -> host amdxdna -> GERCEK NPU
```

Bizim istedigimiz:

```
QEMU XDNA aygiti -> XDNA emulatoru -> host CPU
```

Host'ta NPU yok. Dolayisiyla virtio-npu bu problemi cozmuyor.

## Performans

Ilk emulator cok yavas olabilir (gercekte 10 ms olan is 20 saniye surebilir).
Bu basarisizlik degil. Bir emulatorun ilk hedefi **dogrulugtur**. Sonrasinda
sirasiyla: interpreter -> temel blok onbellegi -> JIT -> host SIMD (AVX2 /
AVX-512). Ileride GPU varsa XDNA operasyonlari CUDA/ROCm'a tasinabilir, ama
GPU projenin gereksinimi degil.

## Yan fayda: istedigin kadar sanal NPU

Fiziksel NPU olmadigi icin her biri kendi tile durumu, bellegi, context'leri,
firmware durumu ve PCI aygitiyla birden fazla sanal NPU olusturulabilir.
Sinir yalnizca host CPU kapasitesidir.
