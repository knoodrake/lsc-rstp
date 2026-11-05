#!/usr/bin/env python3
"""Genere sub_patch/anyka_ipc_vL = vI2 + serialisation des deux tx par le vrai
mutex video 0x4D2AD8.

Acquis static-expert (sub_patch/VK_LOCK_ANALYSIS.md) :
  - cb1 tourne DANS le thread chn1 qui tient deja 0x4D2AD8 (lock 0x7AC4C ...
    cb1 ... unlock 0x7AEF0) => une reprise de verrou bloquante = deadlock (vK2),
    trylock = toujours EBUSY (vK muet). Donc cb1 = tx SANS verrou (code vI2).
  - cb0 (0x8F41C) tourne dans le worker chn0, PAS sous 0x4D2AD8 => vI2 avait
    2 tx concurrents (session1 par chn0, session2 par chn1) = candidat crash.
  => vL : on enveloppe l'ENREGISTREMENT cb0 chn0 par un wrapper cave qui prend
     0x4D2AD8, appelle le cb0 usine, relache. Chn1 tient deja le verrou pendant
     son cb1 => les 2 tx sont serialises, aucun nouveau mutex.

Regions patchees (file offsets, md5 usine 44c39f43…) :
  0x07F728    3 o  bl 0x7BFA0 -> bl 0x49A690 (caveInit)
  0x48A690   80 o  caveInit (vH ; literal chn0 0x8F41C -> 0x49A6E0)
  0x48A6E0  <=96 o wrapper cb0 (build/cb0_wrap.s -> build/cb0_wrap.bin, 64 o)
  0x48A740  ~96 o  cb1 vI2 sans verrou (build/cb1_vI2.bin)
"""
import struct, hashlib, pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
FAC = ROOT / "tuya_keys/cam3_anyka_ipc"
OUT = ROOT / "sub_patch/anyka_ipc_vL"
WRAP = ROOT / "sub_patch/build/cb0_wrap.bin"
CB1 = ROOT / "sub_patch/build/cb1_vI2.bin"
MD5_FAC = "44c39f43b3cc8197c3f63cad0717817f"
MD5_VL = "@@"
CAVEINIT_VA = 0x49A690
WRAP_VA = 0x49A6E0
CB1_VA = 0x49A740
GAP_MAX = 0x48A740 - 0x48A6E0          # 96 o
CB_MAX = 140
CAVEINIT = bytearray(bytes.fromhex(
    "10402de930409fe5040194e52c109fe5"
    "2c209fe50130a0e3e5c3f9eb100184e5"
    "0000a0e31c109fe53886efeb0100a0e3"
    "14109fe53586efeb1080bde814b05200"
    "d0443400044000001cf4080040a74900"
))
BL_TARGETS = [0x11A58C, 0x8F41C, 0x11A5A0]   # lock, factory cb0, unlock


def enc_bl(addr, target):
    return (0xEB000000 | (((target - (addr + 8)) >> 2) & 0xFFFFFF)) & 0xFFFFFFFF


def main():
    fac = bytearray(FAC.read_bytes())
    assert hashlib.md5(fac).hexdigest() == MD5_FAC, "binaire usine inattendu"
    wrap = bytearray(WRAP.read_bytes())
    cb1 = bytearray(CB1.read_bytes())
    assert 0 < len(wrap) <= GAP_MAX and len(cb1) <= CB_MAX, (len(wrap), len(cb1))
    # 1) redirection du setter chn0 vers caveInit (site 0x7F728)
    site = 0x7F728
    w = enc_bl(site + 0x10000, CAVEINIT_VA)
    assert fac[site + 3] == (w >> 24) & 0xFF
    fac[site:site + 3] = struct.pack("<I", w)[:3]
    # 2) caveInit : le literal chn0 (rel 0x48) pointe le wrapper au lieu de l'usine
    assert CAVEINIT[0x48:0x4C] == bytes.fromhex("1cf40800")
    CAVEINIT[0x48:0x4C] = struct.pack("<I", WRAP_VA)
    # 3) bl placeholder du wrapper (ordre source : lock, factory, unlock)
    offs = [i for i in range(0, len(wrap) - 3, 4) if wrap[i:i + 4] == b"\x00\x00\x00\xeb"]
    assert len(offs) == len(BL_TARGETS), (offs, BL_TARGETS)
    for off, target in zip(offs, BL_TARGETS):
        bw = enc_bl(WRAP_VA + off, target)
        assert (bw >> 24) == 0xEB
        wrap[off:off + 4] = struct.pack("<I", bw)
    # 4) bl placeholder du cb1 (tx_video uniquement)
    offs1 = [i for i in range(0, len(cb1) - 3, 4) if cb1[i:i + 4] == b"\x00\x00\x00\xeb"]
    assert len(offs1) == 1, offs1
    bw = enc_bl(CB1_VA + offs1[0], 0x30FB98)
    assert (bw >> 24) == 0xEB
    cb1[offs1[0]:offs1[0] + 4] = struct.pack("<I", bw)
    # 5) zones vierges cote usine
    assert set(fac[0x48A690:0x48A6E0]) == {0}
    assert set(fac[0x48A6E0:0x48A740]) == {0}
    assert set(fac[0x48A740:0x48A740 + CB_MAX]) == {0}
    fac[0x48A690:0x48A690 + len(CAVEINIT)] = CAVEINIT
    fac[0x48A6E0:0x48A6E0 + len(wrap)] = wrap
    fac[0x48A740:0x48A740 + len(cb1)] = cb1
    md5 = hashlib.md5(fac).hexdigest()
    if MD5_VL.startswith("@@"):
        print("md5 calcule", md5)
    else:
        assert md5 == MD5_VL, f"md5 {md5} != {MD5_VL}"
    OUT.write_bytes(fac)
    print(f"OK {OUT.relative_to(ROOT)} md5={md5} taille={len(fac)} wrap={len(wrap)} cb1={len(cb1)}")


if __name__ == "__main__":
    main()
