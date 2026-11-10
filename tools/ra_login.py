"""Signs in to RetroAchievements once and keeps the session token (never the password) for the port.

    python3 -I tools/ra_login.py

Asks for the user name and the password in the terminal (the password isn't shown, nor written anywhere), sends them
only to retroachievements.org, and writes ~/.local/share/gow-psp/retroachievements.ini (user and token, readable by
you alone). Phase 10 of docs/linux.md: the achievements need the token, as RetroAchievements' own clients do.
"""
import getpass
import json
import os
import sys
import urllib.parse
import urllib.request
from pathlib import Path

USER_AGENT = "GoWPSPRecomp/0.1 (Linux)"
OUT = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "gow-psp" / "retroachievements.ini"


def main():
    user = input("RetroAchievements user: ").strip()
    password = getpass.getpass("Password (not shown): ")
    body = urllib.parse.urlencode({"r": "login2", "u": user, "p": password}).encode()
    req = urllib.request.Request("https://retroachievements.org/dorequest.php", data=body, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            answer = json.load(r)
    except (OSError, ValueError) as e:
        print(f"could not reach RetroAchievements: {e}")
        return 1
    if not answer.get("Success") or not answer.get("Token"):
        print("sign-in refused: " + str(answer.get("Error") or answer))
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(OUT, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(f"user={answer.get('User') or user}\ntoken={answer['Token']}\n")
    os.chmod(OUT, 0o600)
    print(f"signed in as {answer.get('User') or user}; token kept in {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
