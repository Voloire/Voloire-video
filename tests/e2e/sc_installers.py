"""Install, run, uninstall: both installers, silently, without elevation."""
import os
import subprocess
import time
import winreg
from pathlib import Path

from vvlib import REPO, Pipe, Report, decodes_clean


def _arp_has(prefix):
    """True when an Apps & features entry whose display name starts with prefix exists."""
    for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            key = winreg.OpenKey(hive, r"Software\Microsoft\Windows\CurrentVersion\Uninstall")
        except OSError:
            continue
        for i in range(winreg.QueryInfoKey(key)[0]):
            try:
                sub = winreg.OpenKey(key, winreg.EnumKey(key, i))
                if str(winreg.QueryValueEx(sub, "DisplayName")[0]).startswith(prefix):
                    return True
            except OSError:
                pass
    return False


def run(out):
    rep = Report("installers")
    dist = REPO / "dist"
    exes = sorted(dist.glob("voloire-video-setup-*.exe"))
    msis = sorted(dist.glob("voloire-video-*.msi"))
    rep.check("dist/ holds the .exe and the .msi", len(exes) == 1 and len(msis) == 1, f"{len(exes)} exe, {len(msis)} msi")
    if not (exes and msis):
        return rep
    d = out / "installers"
    d.mkdir(parents=True, exist_ok=True)
    programs = Path(os.environ["APPDATA"]) / "Microsoft" / "Windows" / "Start Menu" / "Programs"

    def verify_install(label, root):
        need = ["voloire-video.exe", "vvcli.exe", "avcodec-62.dll", "Qt6Widgets.dll", "vcruntime140.dll", "msvcp140.dll",
                "platforms/qwindows.dll", "fonts/Ubuntu-Regular.ttf", "fonts/UFL.txt", "LICENSE", "THIRD-PARTY.md"]
        missing = [n for n in need if not (root / n).exists()]
        rep.check(f"{label}: every runtime file installed", not missing, missing)
        v = subprocess.run([str(root / "vvcli.exe"), "--version"], capture_output=True, text=True)
        rep.check(f"{label}: installed vvcli runs", v.returncode == 0 and v.stdout.strip() != "", v.stdout.strip())
        o = d / f"rec_{label}"
        r = subprocess.run([str(root / "vvcli.exe"), "record", "--synthetic", "--duration", "3", "--fps", "30", "--out", str(o),
                            "--name", "inst"], capture_output=True, text=True, cwd=str(d))
        files = list(o.glob("inst_*.mkv"))
        rep.check(f"{label}: installed build records end to end", r.returncode == 0 and len(files) == 1, r.stderr[-150:])
        if files:
            rep.check(f"{label}: recording decodes", decodes_clean(files[0])[0])
        pipe = f"vv-inst-{label}"
        gui = subprocess.Popen([str(root / "voloire-video.exe"), "--pipe", pipe],
                               env={**os.environ, "VV_SETTINGS_DIR": str(d / ("set_" + label))})
        try:
            c = Pipe(pipe, wait=20)
            st = c.call(cmd="ui_state")
            rep.check(f"{label}: installed GUI shows its tray icon", st.get("trayVisible") and st.get("icon") == "idle", st.get("icon"))
            rep.check(f"{label}: panel shown at launch", st.get("panelVisible"))
            c.call(cmd="shutdown")
            c.close()
            gui.wait(15)
            rep.check(f"{label}: GUI exits cleanly", gui.returncode == 0, gui.returncode)
        except Exception as e:
            rep.check(f"{label}: installed GUI answers", False, e)
        finally:
            if gui.poll() is None:
                gui.kill()

    # ---- Inno Setup .exe
    root = d / "inno"
    r = subprocess.run([str(exes[0]), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", f"/DIR={root}",
                        f"/LOG={d / 'inno.log'}"], timeout=300)
    rep.check("exe: silent install succeeds without elevation", r.returncode == 0, f"rc={r.returncode}")
    rep.check("exe: registered in Apps & features", _arp_has("Voloire-video"))
    lnk = programs / "Voloire-video.lnk"
    rep.check("exe: Start menu shortcut created", lnk.exists(), lnk)
    verify_install("exe", root)
    un = root / "unins000.exe"
    rep.check("exe: uninstaller present", un.exists())
    if un.exists():
        subprocess.run([str(un), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=120)
        time.sleep(3)
        rep.check("exe: uninstall removes the application", not (root / "voloire-video.exe").exists())
        rep.check("exe: uninstall removes the shortcut", not lnk.exists())
        rep.check("exe: no longer registered", not _arp_has("Voloire-video"))

    # ---- WiX .msi
    root = d / "msi"
    r = subprocess.run(["msiexec", "/i", str(msis[0]), "/qn", "/norestart", f"INSTALLFOLDER={root}", "/l*v", str(d / "msi.log")], timeout=300)
    rep.check("msi: silent per-user install succeeds without elevation", r.returncode == 0, f"rc={r.returncode} (log {d / 'msi.log'})")
    lnk2 = programs / "Voloire-video" / "Voloire-video.lnk"
    rep.check("msi: Start menu shortcut created", lnk2.exists(), lnk2)
    rep.check("msi: registered in Apps & features", _arp_has("Voloire-video"))
    verify_install("msi", root)
    r = subprocess.run(["msiexec", "/x", str(msis[0]), "/qn", "/norestart", "/l*v", str(d / "msi-uninstall.log")], timeout=300)
    time.sleep(2)
    rep.check("msi: uninstall succeeds", r.returncode == 0, f"rc={r.returncode}")
    rep.check("msi: uninstall removes the application files", not (root / "voloire-video.exe").exists())
    rep.check("msi: uninstall removes the shortcut", not lnk2.exists())
    rep.check("msi: no longer registered", not _arp_has("Voloire-video"))
    return rep
