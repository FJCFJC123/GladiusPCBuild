#!/usr/bin/env python3
"""GladiusPort Builder - point it at a Gladius (Xbox) disc and get a native Windows PC install.

The retail default.xbe is converted to a Windows exe (tools/xbe2pe.py: no emulation, the Xbox kernel/XAPI/D3D
are replaced by gladiusport.dll) next to the disc files it reads.

GUI:  GladiusPortBuilder.exe
CLI:  GladiusPortBuilder.exe --cli --source Gladius.iso --out <install dir> [--update] [--no-dev-menus] ...
"""
import argparse
import hashlib
import importlib
import json
import os
import queue
import shutil
import socket
import struct
import sys
import threading
import traceback
from pathlib import Path

RETAIL_XBE_MD5 = "3d2761bec7d5450db7dcaa98f50244fd"     # Gladius (USA) default.xbe, 3,874,816 bytes
BUILD_MARKER = ".gladiusport_build"
SETTINGS = Path.home() / ".gladiusport_builder.json"
DEFAULT_RELAY = "161.35.1.158:27961"
SECTOR = 2048
# XDVDFS game partition offsets: plain xiso, XGD1 (redump), XGD2, XGD3
ISO_BASES = (0, 0x18300000, 0xFD90000, 0x2080000)


def bundle_dir():
    """Payload root: PyInstaller temp folder when frozen, else builder/payload (filled by build_exe.bat) or the
    GladiusPort tree itself."""
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS)
    here = Path(__file__).resolve().parent
    if (here / "payload" / "gladiusport.dll").exists():
        return here / "payload"
    return here.parent                                   # dev: GladiusPort\ (tools, build)


def payload_file(name):
    root = bundle_dir()
    roots = [root]
    if not getattr(sys, "frozen", False):               # from source: a stale builder\payload may lack newer files
        roots.append(Path(__file__).resolve().parent.parent)
    for cand in (r / sub / name for r in roots for sub in (".", "build", "netplay", "third_party/SDL2")):
        if cand.exists():
            return cand
    raise FileNotFoundError(f"{name} is missing from the builder payload")


def load_tools():
    """Import xbe2pe / xiso from the shipped tools folder."""
    tools = str(bundle_dir() / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    return importlib.import_module("xbe2pe"), importlib.import_module("xiso")


class Cancelled(Exception):
    pass


class Builder:
    def __init__(self, source, out, options, log=print, progress=None, cancel=None):
        self.source = Path(source) if source else None
        self.out = Path(out)
        self.opt = options
        self.log = log
        self.progress = progress or (lambda done, total, label: None)
        self.cancel = cancel or threading.Event()

    def check(self):
        if self.cancel.is_set():
            raise Cancelled()

    # ------------------------------------------------------------------ disc
    def open_iso(self, xiso):
        for base in ISO_BASES:
            try:
                iso = xiso.XIso(str(self.source), base)
                self.log(f"Disc image: XDVDFS at offset 0x{base:X}")
                return iso
            except (AssertionError, OSError, struct.error):
                continue
        raise RuntimeError(f"{self.source.name} is not an Xbox disc image (no XDVDFS volume found)")

    def extract(self, xiso):
        if self.source.is_dir():
            if not (self.source / "default.xbe").exists() or not (self.source / "gladius.bec").exists():
                raise RuntimeError(f"{self.source} has no default.xbe / gladius.bec - not an extracted Gladius disc")
            files = [p for p in self.source.rglob("*") if p.is_file()]
            total = sum(p.stat().st_size for p in files) or 1
            done = 0
            self.log(f"Copying disc files from {self.source}")
            for p in files:
                self.check()
                dst = self.out / p.relative_to(self.source)
                if self.opt.get("force_extract") or not dst.exists() or dst.stat().st_size != p.stat().st_size:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(p, dst)
                done += p.stat().st_size
                self.progress(done, total, "disc")
            return
        iso = self.open_iso(xiso)
        entries = list(iso.walk())
        names = {p.rstrip("/").lower() for p, _, _, _ in entries}
        if "default.xbe" not in names or "gladius.bec" not in names:
            raise RuntimeError("This disc has no default.xbe / gladius.bec - is it Gladius?")
        total = sum(sz for _, _, sz, d in entries if not d) or 1
        done = 0
        self.log(f"Extracting {len(entries)} disc entries ({total / 2**20:,.0f} MB)")
        for path, sec, sz, is_dir in entries:
            self.check()
            dst = self.out / path
            if is_dir:
                dst.mkdir(parents=True, exist_ok=True)
                continue
            if not self.opt.get("force_extract") and dst.exists() and dst.stat().st_size == sz:
                done += sz
                self.progress(done, total, "disc")
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            with open(dst, "wb") as o:
                left = sz
                while left:
                    self.check()
                    n = min(left, 4 * 2**20)
                    o.write(iso.read(sec, n))
                    sec += n // SECTOR
                    left -= n
                    done += n
                    self.progress(done, total, "disc")
        iso.f.close()

    # ------------------------------------------------------------------ exe
    def verify_xbe(self):
        xbe = self.out / "default.xbe"
        md5 = hashlib.md5(xbe.read_bytes()).hexdigest()
        if md5 != RETAIL_XBE_MD5:
            raise RuntimeError(f"default.xbe is not the supported retail Gladius (USA) executable (md5 {md5}).\n"
                               "GladiusPort is built against that exact executable.")
        self.log("default.xbe: retail Gladius (USA) - OK")

    def convert(self, xbe2pe):
        self.log("Converting default.xbe -> gladius.exe")
        exe = self.out / "gladius.exe"
        xbe2pe.build(str(self.out / "default.xbe"), str(exe), gui=not self.opt.get("console"))
        # the level editor is the same exe under its own name (optional; an editor from an earlier install is removed)
        editor = self.out / "gladius_editor.exe"
        if self.opt.get("editor"):
            shutil.copyfile(exe, editor)
            self.log("Installed gladius_editor.exe (level editor)")
        elif editor.exists():
            editor.unlink()
            self.log("Removed gladius_editor.exe (level editor not selected)")
        layout = self.out / "gladius.layout.txt"
        if layout.exists():
            layout.unlink()
        for name in ("gladiusport.dll", "SDL2.dll", "oldhud.dll"):
            shutil.copyfile(payload_file(name), self.out / name)
            self.log(f"Installed {name}")
        # online versus: the prebuilt gladiusnet.dll; without it the game runs offline
        try:
            shutil.copyfile(payload_file("gladiusnet.dll"), self.out / "gladiusnet.dll")
            # the dll may only be redistributed together with its license
            shutil.copyfile(payload_file("gladiusnet_LICENSE.txt"), self.out / "gladiusnet_LICENSE.txt")
            self.log("Installed gladiusnet.dll (online versus)")
        except FileNotFoundError:
            (self.out / "gladiusnet.dll").unlink(missing_ok=True)
            (self.out / "gladiusnet_LICENSE.txt").unlink(missing_ok=True)
            self.log("gladiusnet.dll not in this builder - online versus unavailable")
        # GladiusPort + third-party license texts travel with every install (THIRD_PARTY_NOTICES.md)
        lic = bundle_dir() / "LICENSES"
        if lic.is_dir():
            shutil.copytree(lic, self.out / "LICENSES", dirs_exist_ok=True)
            shutil.copyfile(bundle_dir() / "THIRD_PARTY_NOTICES.md", self.out / "THIRD_PARTY_NOTICES.md")
            shutil.copyfile(bundle_dir() / "LICENSE", self.out / "LICENSE.txt")
            self.log("Installed licenses (LICENSE.txt, LICENSES, THIRD_PARTY_NOTICES.md)")

    # ------------------------------------------------------------------ settings
    def write_settings(self):
        ini = self.out / "gladiusport.ini"
        self.had_preset = ini_has_preset(ini)
        if not ini.exists():
            ini.write_text("; GladiusPort settings (GladiusPort Builder). Delete this file to get the defaults back.\n"
                           "[cpu]\n; 1 = all game threads on one core like the Xbox, 0 = all cores (experimental)\nSingleCore=1\n\n"
                           "[input]\n; auto = keyboard only while no controller is connected | on | off\nKeyboard=auto\n\n"
                           "[game]\n\n[net]\n; online versus (Versus menu: Host / Join)\nNetplay=1\n", encoding="ascii")
        set_ini(ini, "game", "UnlockVersus", "1" if self.opt.get("unlock_versus", True) else "0")
        set_ini(ini, "game", "DevMenus", "1" if self.opt.get("dev_menus") else "0")
        set_ini(ini, "game", "UnlockArenas", "1")          # all 15 versus arenas (must match across netplay PCs)
        codes = self.opt.get("codes")                       # None (CLI without --codes): leave [codes] as it is
        if codes is not None:
            on = [k for k, _, _ in CODES if k in codes]
            for k, _, x in CODES:
                set_ini(ini, "codes", k, "1" if k in on and not (x in on and CODES_ORDER[x] < CODES_ORDER[k]) else "0")
            self.log("Codes: " + (", ".join(on) if on else "none"))
        # netplay: game memory at fixed addresses on every PC (needed to join a session)
        set_ini(ini, "mem", "Container", "1")
        set_ini(ini, "mem", "StateSync", "1")
        # graphics preset: "recommended" (detected), low / medium / high or "keep"; default: recommended for a new install,
        # keep once the install has a preset
        preset = self.opt.get("preset") or ("keep" if self.had_preset else "recommended")
        hw = detect_hardware()
        self.log("Hardware: " + describe_hardware(hw))
        if preset != "keep":
            tier = hw["tier"] if preset == "recommended" else preset
            write_preset(ini, tier, hw)
            self.log(f"Graphics preset: {tier.capitalize()}" + (" (recommended)" if tier == hw["tier"] else ""))
        else:
            set_ini(ini, "video", "Recommended", hw["tier"])
            self.log("Graphics settings kept (in game: Options -> Video -> Preset)")
        net = self.out / "gladiusnet.ini"
        if not net.exists():
            net.write_text("[net]\n; online versus relay server (match list + packet relay), host:port\n"
                           f"relay={DEFAULT_RELAY}\n; lan=1 also lists matches on the local network\nlan=1\n"
                           "; name shown to other players\nname=Player\n[test]\nnetsim=0\n"
                           "[sim]\n; AI Simulator: versus teams become prebuilt league schools played by the AI\n"
                           "; (the host's settings apply to everyone; also: Versus popup -> LB/RB Network Test)\n"
                           "Enabled=0\n; level range of every simulated gladiator (1-30)\nLevelMin=12\nLevelMax=18\n"
                           "; optional comma list of schools (names from data\\units\\unitschools.idx); empty = all 95\n"
                           "Schools=\n", encoding="ascii")
        # HD texture pack: a folder of <texture name>.png (empty = leave the install's setting as it is)
        pack = (self.opt.get("texture_pack") or "").strip()
        if pack:
            folder = Path(pack).resolve()
            if not folder.is_dir():
                raise RuntimeError(f"HD texture pack folder not found: {folder}")
            pngs = sum(1 for _ in folder.glob("*.png"))
            if not pngs:
                raise RuntimeError(f"No .png textures in {folder} - choose the folder that holds the pack's images")
            set_ini(ini, "textures", "Pack", str(folder))
            self.log(f"HD texture pack: {folder} ({pngs} textures; F4 in game toggles it)")
        if self.opt.get("player_name"):
            set_ini(net, "net", "name", self.opt["player_name"][:15])
        if self.opt.get("relay"):
            set_ini(net, "net", "relay", self.opt["relay"])
        # netplay autotest launchers (dev gladiusnet.dll only; a release build ignores -gnauto, so stale ones are removed)
        net_dll = self.out / "gladiusnet.dll"
        release = not net_dll.exists() or b"GNBUILD=release" in net_dll.read_bytes()
        for name, mode in (("Netplay Autotest - Host.bat", "host"), ("Netplay Autotest - Join.bat", "join")):
            if release:
                (self.out / name).unlink(missing_ok=True)
            else:
                (self.out / name).write_bytes(b'@echo off\r\ncd /d "%~dp0"\r\nstart "" gladius.exe -gnauto='
                                              + mode.encode() + b'\r\n')
        self.log("Settings: gladiusport.ini, gladiusnet.ini" + ("" if release else " (+ autotest launchers)"))

    # ------------------------------------------------------------------ mods
    def apply_mod(self):
        """Mods tab: one mod (a folder of xdelta patches laid out like the install: gladius.bec.xdelta,
        data\\Audio\\...\\X.FLO.xdelta) applied to the install. The originals are kept in mods_backup\\ and put back
        before another mod (or none) is applied, so switching always starts from the unmodded files."""
        name = self.opt.get("mod")                           # None (CLI without --mod): leave the install as it is
        if name is None:
            return
        backup = self.out / MOD_BACKUP
        state_file = backup / "mod.json"
        try:
            state = json.loads(state_file.read_text())
        except (OSError, ValueError):
            state = {"mod": "", "files": []}
        patches = []
        if name:
            folder = Path(self.opt.get("xdelta_dir") or "") / name
            patches = sorted(folder.rglob("*.xdelta")) if folder.is_dir() else []
            if not patches:
                raise RuntimeError(f"No .xdelta patches in {folder}")
        for rel in state["files"]:                           # back to the original files (a Build may have
                                                             # re-extracted some already: same bytes)
            src = backup / rel
            if src.exists():
                shutil.copyfile(src, self.out / rel)
                src.unlink()
        if state["files"]:
            self.log(f"Mod removed: {state['mod']} ({len(state['files'])} files restored)")
        if not name:
            shutil.rmtree(backup, ignore_errors=True)
            return
        state = {"mod": "", "files": []}
        state_file.parent.mkdir(parents=True, exist_ok=True)
        state_file.write_text(json.dumps(state))
        vcdiff = importlib.import_module("vcdiff")
        self.log(f"Applying mod: {name} ({len(patches)} patches)")
        try:
            for i, p in enumerate(patches):
                self.check()
                rel = p.relative_to(folder).with_suffix("").as_posix()
                target = self.out / rel
                if not target.exists():
                    raise RuntimeError(f"{name}: {rel} is not in the install (patch {p.name})")
                orig = backup / rel
                orig.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(target, orig)
                state["files"].append(rel)
                state_file.write_text(json.dumps(state))
                try:
                    vcdiff.apply(str(orig), str(p), str(target))
                except vcdiff.PatchError as e:
                    raise RuntimeError(f"{name}: {rel}: {e}") from None
                self.log(f"  patched {rel}")
                self.progress(i + 1, len(patches), "mod")
        except BaseException:
            for rel in state["files"]:                       # leave the install unmodded, not half patched
                if (backup / rel).exists():
                    shutil.copyfile(backup / rel, self.out / rel)
                    (backup / rel).unlink()
            state_file.write_text(json.dumps({"mod": "", "files": []}))
            raise
        state["mod"] = name
        state_file.write_text(json.dumps(state))
        self.log(f"Mod applied: {name} (originals kept in {MOD_BACKUP}\\ - choose None to remove it)")

    # ------------------------------------------------------------------ run
    def run(self, update=False):
        xbe2pe, xiso = load_tools()
        self.out.mkdir(parents=True, exist_ok=True)
        if update:
            if not (self.out / "default.xbe").exists():
                raise RuntimeError(f"{self.out} is not a GladiusPort install (no default.xbe) - do a full build")
        else:
            if not self.source or not self.source.exists():
                raise RuntimeError("Choose the Gladius disc image (.iso) or an extracted disc folder")
            self.extract(xiso)
        self.check()
        self.verify_xbe()
        self.convert(xbe2pe)
        self.write_settings()
        self.apply_mod()
        (self.out / BUILD_MARKER).write_text("GladiusPort install created by GladiusPort Builder\n")
        self.log(f"Done. Play: {self.out / 'gladius.exe'}")


# Gameplay codes ([codes] in gladiusport.ini, src/codes.cpp): ports of the GameCube Gecko codes. (key, label, the key it
# can't be combined with). They change gameplay, so online players need the same set (part of the netplay version check).
CODES = [
    ("ExpandedClasses", "Expanded SkillUseClass table (CustomUnit1-32 classes for ClassDef / Skills .tok)", None),
    ("Roster40", "40-gladiator school roster", "Unlock20Slots"),
    ("AnimPool", "Larger animation pool (4000 -> 32000 loaded animations)", None),
    ("PreserveJP", "Recruits keep the JP their level earned", "InfiniteJP"),
    ("InfiniteJP", "Infinite JP for recruited gladiators", "PreserveJP"),
    ("InfiniteMoney", "Infinite money", None),
    ("PoisonPercent", "Poison deals 10% of max health", None),
    ("BleedPercent", "Bleeding deals 10% of max health", None),
    ("Unlock20Slots", "All 20 school slots unlocked from the start", "Roster40"),
    ("OldHud", "Old-style battle HUD (from the Feb 2003 PS2 demo; visual only, any online opponent)", None),
]
CODES_ORDER = {k: i for i, (k, _, _) in enumerate(CODES)}

# Mods tab: the install's originals of the files a mod's patches change, and mod.json (which mod, which files)
MOD_BACKUP = "mods_backup"


def list_mods(xdelta_dir):
    """The mods in an Xdelta folder: its subfolders that hold .xdelta patches"""
    try:
        return sorted(d.name for d in Path(xdelta_dir).iterdir() if d.is_dir() and any(d.rglob("*.xdelta")))
    except OSError:
        return []


def installed_mod(out):
    try:
        return json.loads((Path(out) / MOD_BACKUP / "mod.json").read_text()).get("mod", "")
    except (OSError, ValueError):
        return ""


def set_ini(path, section, key, value):
    """Set one key, keeping everything else (comments, other keys) as it is."""
    lines = Path(path).read_text(encoding="latin-1").splitlines()
    sec, insert_at, found = None, None, False
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            if sec == section.lower() and insert_at is None:
                insert_at = i
            sec = s[1:-1].strip().lower()
            continue
        if sec == section.lower() and "=" in s and not s.startswith(";"):
            if s.split("=", 1)[0].strip().lower() == key.lower():
                lines[i] = f"{key}={value}"
                found = True
    if not found:
        if sec == section.lower() and insert_at is None:
            lines.append(f"{key}={value}")
        elif insert_at is not None:
            while insert_at > 0 and not lines[insert_at - 1].strip():
                insert_at -= 1
            lines.insert(insert_at, f"{key}={value}")
        else:
            lines += ["", f"[{section}]", f"{key}={value}"]
    Path(path).write_text("\n".join(lines) + "\n", encoding="latin-1")


# Graphics presets written to gladiusport.ini at install: the same table as src/options_video.cpp k_preset (keep
# them equal). Screen Fit / UI Aspect are left alone; CPU Cores = Multi (SingleCore=0).
TIERS = ("low", "medium", "high")
PRESETS = {
    "AntiAlias": ("fxaa", "taa", "taa"), "Lighting": ("vertex", "pixel", "pixel"),
    "Shadows": ("0", "1", "1"), "SSAO": ("0", "1", "1"), "SoftParticles": ("1", "1", "1"),
    "Bloom": ("0.3", "0.6", "1.0"), "GodRays": ("0", "0.5", "1.0"), "FxLights": ("0.3", "0.6", "1.0"),
    "Materials": ("1.0", "1.0", "1.0"), "RenderScale": ("1", "1", "1"),
    "ShadowMapSize": ("2048", "4096", "8192"), "HDR": ("1", "1", "1"), "Resolution": ("auto", "auto", "auto"),
}


def detect_hardware():
    """Strongest display adapter (name, vendor, video memory) from the display-class registry keys, CPU cores and
    the primary monitor's mode -> preset tier (same rules as src/hw_detect.cpp)."""
    import ctypes
    import winreg
    hw = {"gpu": "unknown GPU", "vendor": 0, "vram_mb": 0, "cores": os.cpu_count() or 1,
          "threads": os.cpu_count() or 1, "screen_w": 0, "screen_h": 0}
    cls = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, cls) as root:
            for i in range(64):
                try:
                    sub = winreg.EnumKey(root, i)
                except OSError:
                    break
                if not sub.isdigit():
                    continue
                try:
                    with winreg.OpenKey(root, sub) as k:
                        def val(name):
                            try:
                                return winreg.QueryValueEx(k, name)[0]
                            except OSError:
                                return None
                        name = val("DriverDesc") or ""
                        dev = (val("MatchingDeviceId") or "").lower()
                        mem = val("HardwareInformation.qwMemorySize")
                        if mem is None:
                            mem = val("HardwareInformation.MemorySize")
                        if isinstance(mem, bytes):
                            mem = int.from_bytes(mem[:8], "little")
                        mem = int(mem or 0)
                        if not name or "basic display" in name.lower() or "basic render" in name.lower():
                            continue
                        vendor = int(dev.split("ven_")[1][:4], 16) if "ven_" in dev else 0
                        if mem >> 20 > hw["vram_mb"] or not hw["vendor"]:
                            hw.update(gpu=name, vendor=vendor, vram_mb=mem >> 20)
                except (OSError, ValueError):
                    continue
    except OSError:
        pass
    try:                                                    # physical cores
        class SLPI(ctypes.Structure):
            _fields_ = [("mask", ctypes.c_size_t), ("rel", ctypes.c_int), ("pad", ctypes.c_ulonglong * 2)]
        k32 = ctypes.windll.kernel32
        n = ctypes.c_ulong(0)
        k32.GetLogicalProcessorInformation(None, ctypes.byref(n))
        count = n.value // ctypes.sizeof(SLPI)
        buf = (SLPI * count)()
        if count and k32.GetLogicalProcessorInformation(buf, ctypes.byref(n)):
            cores = sum(1 for e in buf if e.rel == 0)   # RelationProcessorCore
            if cores:
                hw["cores"] = cores
    except Exception:                                       # noqa: BLE001
        pass
    try:                                                    # monitor mode in real pixels (not DPI-scaled)
        class DEVMODEW(ctypes.Structure):
            _fields_ = [("name", ctypes.c_wchar * 32), ("spec", ctypes.c_ushort), ("drv", ctypes.c_ushort),
                        ("size", ctypes.c_ushort), ("extra", ctypes.c_ushort), ("fields", ctypes.c_ulong),
                        ("u1", ctypes.c_byte * 16), ("color", ctypes.c_short * 5), ("form", ctypes.c_wchar * 32),
                        ("logpix", ctypes.c_ushort), ("bpp", ctypes.c_ulong), ("w", ctypes.c_ulong),
                        ("h", ctypes.c_ulong), ("u2", ctypes.c_ulong), ("freq", ctypes.c_ulong),
                        ("rest", ctypes.c_byte * 32)]
        dm = DEVMODEW()
        dm.size = ctypes.sizeof(DEVMODEW)
        if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(dm)):   # ENUM_CURRENT_SETTINGS
            hw["screen_w"], hw["screen_h"] = dm.w, dm.h
    except Exception:                                       # noqa: BLE001
        pass
    # tier: video memory; Intel integrated or < 2 GB = Low, < 6 GB = Medium, else High; < 4 cores caps at Medium
    integrated = hw["vendor"] == 0x8086 or hw["vram_mb"] < 1024
    tier = 0 if integrated or hw["vram_mb"] < 2048 else 1 if hw["vram_mb"] < 6144 else 2
    if hw["cores"] < 4 and tier == 2:
        tier = 1
    hw["tier"] = TIERS[tier]
    return hw


def describe_hardware(hw):
    return (f"{hw['gpu']} ({hw['vram_mb'] / 1024:.0f} GB video memory), {hw['cores']} cores / {hw['threads']} "
            f"threads, screen {hw['screen_w']}x{hw['screen_h']} -> {hw['tier'].capitalize()} preset")


def write_preset(ini, tier, hw):
    t = TIERS.index(tier)
    for key, vals in PRESETS.items():
        v = vals[t]
        if key == "Resolution" and tier == "low" and hw.get("screen_h", 0) > 1080:
            v = "640x480"                                   # Low on a big monitor: the original frame
        set_ini(ini, "video", key, v)
    set_ini(ini, "cpu", "SingleCore", "0")                  # CPU Cores = Multi (experimental, see above)
    set_ini(ini, "video", "Preset", tier)
    set_ini(ini, "video", "Recommended", hw["tier"])


def ini_has_preset(ini):
    try:
        return any(l.strip().lower().startswith("preset=")
                   for l in Path(ini).read_text(encoding="latin-1").splitlines())
    except OSError:
        return False


def check_output_dir(out):
    out = Path(out)
    return not out.exists() or not any(out.iterdir()) or (out / BUILD_MARKER).exists() or \
        (out / "default.xbe").exists()


# ======================================================================== CLI
def cli(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", action="store_true")
    ap.add_argument("--source", help="Gladius Xbox .iso, or extracted disc folder (not needed with --update)")
    ap.add_argument("--out", help="Install folder")
    ap.add_argument("--update", action="store_true", help="Only re-create gladius.exe + DLLs (disc files untouched)")
    ap.add_argument("--dev-menus", action="store_true", help="(default) Developer menus: splash school loader, debug menus")
    ap.add_argument("--no-dev-menus", action="store_true", help="Turn the developer menus off")
    ap.add_argument("--no-unlock-versus", action="store_true", help="Versus only after story progress (retail)")
    ap.add_argument("--console", action="store_true", help="Keep a console window next to the game")
    ap.add_argument("--editor", action="store_true", help="Also install gladius_editor.exe (level editor)")
    ap.add_argument("--texture-pack", help="HD texture pack folder (<texture name>.png files)")
    ap.add_argument("--name", help="Player name for online versus")
    ap.add_argument("--relay", help=f"Online relay host:port (default {DEFAULT_RELAY})")
    ap.add_argument("--force-extract", action="store_true", help="Re-extract disc files that already exist")
    ap.add_argument("--force", action="store_true", help="Allow building into a non-empty foreign folder")
    ap.add_argument("--preset", choices=("recommended", "low", "medium", "high", "keep"),
                    help="Graphics preset (default: recommended for a new install, keep once one is set)")
    ap.add_argument("--detect", action="store_true", help="Only show the detected hardware and its preset")
    ap.add_argument("--codes", help="Comma list of gameplay codes to turn on ('none' = all off): "
                    + ", ".join(k for k, _, _ in CODES))
    ap.add_argument("--xdelta-dir", help="Xdelta folder: one subfolder of .xdelta patches per mod")
    ap.add_argument("--mod", help="Mod (subfolder of --xdelta-dir) to apply, 'none' = remove the applied mod")
    a = ap.parse_args(argv)
    if a.detect:
        print("Hardware: " + describe_hardware(detect_hardware()))
        return
    if not a.out:
        sys.exit("--out is required.")
    if not a.update and not a.source:
        sys.exit("--source is required for a full build.")
    if not a.update and not a.force and not check_output_dir(a.out):
        sys.exit(f"{a.out} is not empty and is not a GladiusPort install (use --force).")
    last = {}

    def progress(done, total, label):
        pct = done * 10 // max(1, total)
        if last.get(label) != pct:
            last[label] = pct
            print(f"  [{label}] {pct * 10}%", flush=True)

    opts = {"dev_menus": not a.no_dev_menus, "unlock_versus": not a.no_unlock_versus, "console": a.console,
            "editor": a.editor, "player_name": a.name, "relay": a.relay, "force_extract": a.force_extract, "preset": a.preset,
            "texture_pack": a.texture_pack}
    if a.codes is not None:
        codes = [] if a.codes.strip().lower() == "none" else [c.strip() for c in a.codes.split(",") if c.strip()]
        bad = [c for c in codes if c not in CODES_ORDER]
        if bad:
            sys.exit("unknown code(s): " + ", ".join(bad))
        opts["codes"] = codes
    if a.mod is not None:
        mod = "" if a.mod.strip().lower() == "none" else a.mod.strip()
        if mod and mod not in list_mods(a.xdelta_dir or ""):
            sys.exit(f"no mod '{mod}' in --xdelta-dir (mods: {', '.join(list_mods(a.xdelta_dir or '')) or 'none'})")
        opts["mod"], opts["xdelta_dir"] = mod, a.xdelta_dir
    Builder(a.source, a.out, opts, progress=progress).run(update=a.update)


# ======================================================================== GUI
def gui():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    settings = {}
    try:
        settings = json.loads(SETTINGS.read_text())
    except Exception:
        pass

    root = tk.Tk()
    root.title("GladiusPort Builder")
    root.minsize(640, 520)
    frm = ttk.Frame(root, padding=12)
    frm.pack(fill="both", expand=True)
    frm.columnconfigure(1, weight=1)

    v_src = tk.StringVar(value=settings.get("source", ""))
    v_out = tk.StringVar(value=settings.get("out", str(Path.home() / "Desktop" / "GladiusPC_Native")))
    v_dev = tk.BooleanVar(value=settings.get("dev_menus", True))
    v_vs = tk.BooleanVar(value=settings.get("unlock_versus", True))
    v_con = tk.BooleanVar(value=settings.get("console", False))
    v_edit = tk.BooleanVar(value=settings.get("editor", False))
    v_name = tk.StringVar(value=settings.get("player_name", socket.gethostname()[:15]))
    v_relay = tk.StringVar(value=settings.get("relay", DEFAULT_RELAY))
    v_pack = tk.StringVar(value=settings.get("texture_pack", ""))
    hw = detect_hardware()
    preset_choices = {"Automatic (new install: recommended, else keep)": None,
                      f"Recommended for this PC ({hw['tier'].capitalize()})": "recommended",
                      "Low": "low", "Medium": "medium", "High": "high", "Keep my current settings": "keep"}
    v_preset = tk.StringVar(value=next(iter(preset_choices)))

    ttk.Label(frm, text="GladiusPort Builder", font=("Segoe UI", 14, "bold")).grid(row=0, column=0, columnspan=3,
                                                                                 sticky="w")
    ttk.Label(frm, text="Native Windows build of Gladius from your Xbox disc - no emulator.").grid(
        row=1, column=0, columnspan=3, sticky="w", pady=(0, 10))

    def pick_iso():
        p = filedialog.askopenfilename(title="Gladius Xbox disc image",
                                       filetypes=[("Xbox disc image", "*.iso *.xiso"), ("All files", "*.*")])
        if p:
            v_src.set(p)

    def pick_dir():
        p = filedialog.askdirectory(title="Extracted Gladius disc folder (with default.xbe)")
        if p:
            v_src.set(p)

    def pick_pack():
        p = filedialog.askdirectory(title="HD texture pack folder (<texture name>.png files)")
        if p:
            v_pack.set(os.path.normpath(p))

    def pick_out():
        p = filedialog.askdirectory(title="Install folder")
        if p:
            v_out.set(p)

    nb = ttk.Notebook(frm)
    nb.grid(row=2, column=0, columnspan=3, sticky="ew")
    tab = ttk.Frame(nb, padding=8)
    tab.columnconfigure(1, weight=1)
    nb.add(tab, text="Install")
    ttk.Label(tab, text="Gladius disc").grid(row=0, column=0, sticky="w")
    ttk.Entry(tab, textvariable=v_src).grid(row=0, column=1, sticky="ew", padx=6)
    bsrc = ttk.Frame(tab)
    bsrc.grid(row=0, column=2)
    ttk.Button(bsrc, text="ISO...", command=pick_iso).pack(side="left")
    ttk.Button(bsrc, text="Folder...", command=pick_dir).pack(side="left", padx=(4, 0))
    ttk.Label(tab, text="Install folder").grid(row=1, column=0, sticky="w", pady=4)
    ttk.Entry(tab, textvariable=v_out).grid(row=1, column=1, sticky="ew", padx=6)
    ttk.Button(tab, text="Browse...", command=pick_out).grid(row=1, column=2, sticky="ew")

    box = ttk.LabelFrame(tab, text="Options", padding=8)
    box.grid(row=2, column=0, columnspan=3, sticky="ew", pady=8)
    box.columnconfigure(1, weight=1)
    ttk.Checkbutton(box, text="Versus always available (no story progress needed)", variable=v_vs).grid(
        row=0, column=0, columnspan=2, sticky="w")
    ttk.Checkbutton(box, text="Developer menus (splash school loader, world map Y menu, battle Debug Menu)",
                    variable=v_dev).grid(row=1, column=0, columnspan=2, sticky="w")
    ttk.Checkbutton(box, text="Console window (troubleshooting)", variable=v_con).grid(row=0, column=0, columnspan=2,
                                                                                         sticky="w")
    ttk.Checkbutton(box, text="Level editor (gladius_editor.exe: props, meshes, levels, battle grid)",
                    variable=v_edit).grid(row=3, column=0, columnspan=2, sticky="w")
    ttk.Label(box, text="Online name").grid(row=4, column=0, sticky="w", pady=(6, 0))
    ttk.Entry(box, textvariable=v_name, width=20).grid(row=4, column=1, sticky="w", padx=6, pady=(6, 0))
    ttk.Label(box, text="Online server").grid(row=5, column=0, sticky="w")
    ttk.Entry(box, textvariable=v_relay, width=28).grid(row=5, column=1, sticky="w", padx=6)
    ttk.Label(box, text="Graphics preset").grid(row=6, column=0, sticky="w", pady=(6, 0))
    ttk.Combobox(box, textvariable=v_preset, values=list(preset_choices), state="readonly", width=46).grid(
        row=6, column=1, sticky="w", padx=6, pady=(6, 0))
    ttk.Label(box, text="Detected: " + describe_hardware(hw), foreground="#555").grid(
        row=7, column=0, columnspan=2, sticky="w")
    ttk.Label(box, text="HD texture pack").grid(row=8, column=0, sticky="w", pady=(6, 0))
    fpack = ttk.Frame(box)
    fpack.grid(row=8, column=1, sticky="ew", padx=6, pady=(6, 0))
    fpack.columnconfigure(0, weight=1)
    ttk.Entry(fpack, textvariable=v_pack).grid(row=0, column=0, sticky="ew")
    ttk.Button(fpack, text="Browse...", command=pick_pack).grid(row=0, column=1, padx=(4, 0))
    ttk.Label(box, text="Optional: folder of upscaled <texture name>.png files. Empty = keep the current setting.",
              foreground="#555").grid(row=9, column=0, columnspan=2, sticky="w")


    # Codes tab: one box per ported Gecko code; a box whose partner is ticked is greyed out
    ctab = ttk.Frame(nb, padding=8)
    nb.add(ctab, text="Codes")
    ttk.Label(ctab, text="Gameplay codes (ported from the GameCube Gecko codes). Applied on Build or Update exe only.",
              wraplength=600).grid(row=0, column=0, sticky="w")
    ttk.Label(ctab, text="Online versus only connects players who use the same codes.", foreground="#555").grid(
        row=1, column=0, sticky="w", pady=(0, 6))
    saved_codes = set(settings.get("codes") or [])
    v_codes, c_boxes = {}, {}
    labels = {k: lbl for k, lbl, _ in CODES}

    def code_sync(*_):
        for k, _, x in CODES:
            if x and v_codes[x].get():
                v_codes[k].set(False)
                c_boxes[k].configure(state="disabled", text=f"{labels[k]}  (not with: {labels[x]})")
            else:
                c_boxes[k].configure(state="normal", text=labels[k])

    for i, (k, lbl, _) in enumerate(CODES):
        v_codes[k] = tk.BooleanVar(value=k in saved_codes)
        c_boxes[k] = ttk.Checkbutton(ctab, text=lbl, variable=v_codes[k], command=code_sync)
        c_boxes[k].grid(row=2 + i, column=0, sticky="w", pady=1)
    for k, _, x in CODES:                                   # both saved on: the first in the list wins
        if x and v_codes[k].get() and v_codes[x].get() and CODES_ORDER[x] > CODES_ORDER[k]:
            v_codes[x].set(False)
    code_sync()

    # Mods tab: the subfolders of an Xdelta folder (one mod each, patches laid out like the install); one mod at a time
    mtab = ttk.Frame(nb, padding=8)
    mtab.columnconfigure(1, weight=1)
    nb.add(mtab, text="Mods")
    v_xd = tk.StringVar(value=settings.get("xdelta_dir", str(Path.home() / "Desktop" / "Xdelta")))
    v_mod = tk.StringVar(value=settings.get("mod", ""))
    ttk.Label(mtab, text="GameCube mods ported to the Xbox data, applied as xdelta patches on Build or Update exe. "
              "One mod at a time: the original files are kept and put back before another mod (or None) is applied.",
              wraplength=600).grid(row=0, column=0, columnspan=3, sticky="w")
    ttk.Label(mtab, text="Online versus: both players need the same mod.", foreground="#555").grid(
        row=1, column=0, columnspan=3, sticky="w", pady=(0, 6))
    ttk.Label(mtab, text="Xdelta folder").grid(row=2, column=0, sticky="w")
    ttk.Entry(mtab, textvariable=v_xd).grid(row=2, column=1, sticky="ew", padx=6)
    mlist = ttk.LabelFrame(mtab, text="Mod", padding=8)
    mlist.grid(row=3, column=0, columnspan=3, sticky="ew", pady=8)
    m_info = ttk.Label(mtab, foreground="#555")
    m_info.grid(row=4, column=0, columnspan=3, sticky="w")

    def mods_refresh(*_):
        for w in mlist.winfo_children():
            w.destroy()
        mods = list_mods(v_xd.get().strip())
        if v_mod.get() and v_mod.get() not in mods:
            v_mod.set("")
        ttk.Radiobutton(mlist, text="None (unmodded game)", value="", variable=v_mod).grid(row=0, column=0, sticky="w")
        for i, m in enumerate(mods):
            n = sum(1 for _ in (Path(v_xd.get().strip()) / m).rglob("*.xdelta"))
            ttk.Radiobutton(mlist, text=f"{m}  ({n} patch{'es' if n != 1 else ''})", value=m, variable=v_mod).grid(
                row=1 + i, column=0, sticky="w", pady=1)
        if not mods:
            ttk.Label(mlist, text="No mods found - the Xdelta folder needs one subfolder of .xdelta patches per mod.",
                      foreground="#555").grid(row=1, column=0, sticky="w")
        cur = installed_mod(v_out.get().strip())
        m_info.configure(text=f"Installed now: {cur or 'none'}")

    def pick_xd():
        p = filedialog.askdirectory(title="Xdelta folder (one subfolder per mod)")
        if p:
            v_xd.set(os.path.normpath(p))
            mods_refresh()

    ttk.Button(mtab, text="Browse...", command=pick_xd).grid(row=2, column=2, sticky="ew")
    ttk.Button(mtab, text="Refresh", command=mods_refresh).grid(row=5, column=2, sticky="e", pady=(6, 0))
    mods_refresh()

    bar = ttk.Progressbar(frm, maximum=1000)
    bar.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(8, 0))
    logw = tk.Text(frm, height=12, state="disabled", wrap="word")
    logw.grid(row=6, column=0, columnspan=3, sticky="nsew", pady=8)
    frm.rowconfigure(6, weight=1)
    btns = ttk.Frame(frm)
    btns.grid(row=7, column=0, columnspan=3, sticky="e")

    q = queue.Queue()
    state = {"thread": None, "cancel": threading.Event()}

    def log(msg):
        q.put(("log", msg))

    def progress(done, total, label):
        q.put(("prog", done * 1000 // max(1, total)))

    def pump():
        try:
            while True:
                kind, val = q.get_nowait()
                if kind == "log":
                    logw.configure(state="normal")
                    logw.insert("end", val + "\n")
                    logw.see("end")
                    logw.configure(state="disabled")
                elif kind == "prog":
                    bar["value"] = val
                elif kind == "done":
                    set_busy(False)
                    if val:
                        messagebox.showerror("GladiusPort Builder", val)
                    else:
                        bar["value"] = 1000
                        messagebox.showinfo("GladiusPort Builder", "Build finished.\n\nStart gladius.exe in "
                                            + v_out.get())
        except queue.Empty:
            pass
        root.after(100, pump)

    def opts():
        return {"dev_menus": v_dev.get(), "unlock_versus": v_vs.get(), "console": v_con.get(),
                "editor": v_edit.get(), "player_name": v_name.get().strip(), "relay": v_relay.get().strip(),
                "preset": preset_choices.get(v_preset.get()), "texture_pack": v_pack.get().strip(),
                "codes": [k for k, _, _ in CODES if v_codes[k].get()],
                "mod": v_mod.get(), "xdelta_dir": v_xd.get().strip()}

    def save():
        s = {"source": v_src.get(), "out": v_out.get(), **opts()}
        try:
            SETTINGS.write_text(json.dumps(s, indent=1))
        except OSError:
            pass

    def start(update):
        out = v_out.get().strip()
        if not out:
            messagebox.showerror("GladiusPort Builder", "Choose an install folder")
            return
        if not update and not check_output_dir(out) and not messagebox.askyesno(
                "GladiusPort Builder", f"{out} is not empty and is not a GladiusPort install.\nBuild into it anyway?"):
            return
        save()
        state["cancel"] = threading.Event()
        set_busy(True)
        bar["value"] = 0

        def work():
            err = None
            try:
                Builder(v_src.get().strip(), out, opts(), log=log, progress=progress,
                        cancel=state["cancel"]).run(update=update)
            except Cancelled:
                err = "Cancelled."
            except Exception as e:                          # noqa: BLE001
                log(traceback.format_exc())
                err = str(e)
            q.put(("done", err))

        state["thread"] = threading.Thread(target=work, daemon=True)
        state["thread"].start()

    b_build = ttk.Button(btns, text="Build", command=lambda: start(False))
    b_update = ttk.Button(btns, text="Update exe only", command=lambda: start(True))
    b_cancel = ttk.Button(btns, text="Cancel", command=lambda: state["cancel"].set())
    b_update.pack(side="left", padx=4)
    b_build.pack(side="left", padx=4)
    b_cancel.pack(side="left", padx=4)

    def set_busy(busy):
        for b in (b_build, b_update):
            b.configure(state="disabled" if busy else "normal")
        b_cancel.configure(state="normal" if busy else "disabled")

    set_busy(False)
    log("Choose your Gladius (USA) Xbox disc image and an install folder, then Build.")
    pump()
    root.mainloop()


if __name__ == "__main__":
    if "--cli" in sys.argv:
        if sys.stdout is None:                                  # windowed exe started from a console
            import ctypes
            if ctypes.windll.kernel32.AttachConsole(-1):
                sys.stdout = sys.stderr = open("CONOUT$", "w")
        try:
            cli([a for a in sys.argv[1:]])
        except Exception as e:                                  # noqa: BLE001
            sys.exit(f"error: {e}")
    else:
        gui()
