#!/usr/bin/env bash
# Pre-Flight-Check vor dem ersten "devpod up".
# Nutzung (auf dem Mac mini):
#   ./scripts/devpod-check.sh
#   ./scripts/devpod-check.sh benutzer@mac-studio.local   # inkl. SSH-Test
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()  { echo -e "${GREEN}[ok]${NC} $*"; }
warn(){ echo -e "${YELLOW}[?]${NC} $*"; }
fail(){ echo -e "${RED}[X]${NC} $*"; }

echo "=== DevPod Pre-Flight (Mac mini -> Mac Studio) ==="
echo

# 1) DevPod
if command -v devpod >/dev/null 2>&1; then
  ok "DevPod installiert: $(devpod version 2>/dev/null | head -1 || echo 'devpod')"
else
  fail "DevPod nicht im PATH. Install: https://devpod.sh/docs/getting-started/install"
  exit 1
fi

# 2) SSH-Key
if [ -f ~/.ssh/id_ed25519 ] || [ -f ~/.ssh/id_rsa ]; then
  ok "SSH-Key gefunden"
else
  warn "Kein Standard-SSH-Key (~/.ssh/id_ed25519 oder id_rsa). ggf. ssh-keygen ausführen."
fi

# 3) Optional: SSH-Verbindung zum Remote-Host testen
REMOTE="${1:-}"
if [ -n "$REMOTE" ]; then
  if ssh -o BatchMode=yes -o ConnectTimeout=5 "$REMOTE" "echo ok" 2>/dev/null | grep -q ok; then
    ok "SSH-Verbindung zu $REMOTE funktioniert (passwortlos)"
  else
    warn "SSH zu $REMOTE schlägt fehl oder erfordert Passwort. Bitte Key mit ssh-copy-id einrichten."
  fi
else
  echo "Tipp: SSH testen mit: $0 benutzer@mac-studio.local"
fi

echo
echo "Weitere Schritte: Mac Studio (Docker Desktop, Remote-Anmeldung), dann:"
echo "  devpod provider add ssh -o HOST=USER@MAC_STUDIO_HOST"
echo "  devpod up . --provider ssh"
