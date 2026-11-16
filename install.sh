#!/usr/bin/env bash
set -euo pipefail

REPO_URL="${TESTHDR_REPO_URL:-https://github.com/vedix95/TEST_HDR.git}"
INSTALL_DIR="${TESTHDR_DIR:-$HOME/Work/TEST_HDR}"

echo "============================================"
echo "  TEST_HDR v0.5 — Installation Omarchy"
echo "============================================"

is_omarchy() {
  grep -qiE '^(ID|NAME)=["'\'']?omarchy["'\'']?$' /etc/os-release 2>/dev/null || \
    [[ -x /usr/share/omarchy/bin/omarchy-pkg-aur-add ]]
}

has_monique() {
  command -v monique >/dev/null 2>&1 && \
    python3 -c "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('monique') else 1)" 2>/dev/null
}

# 1. Vérifier les dépendances build & runtime
DEPS=(git cmake gcc pkgconf sdl3 pango cairo wayland wayland-protocols lcms2 shaderc python brightnessctl)
if [[ -x /usr/share/omarchy/bin/omarchy-pkg-add ]]; then
  echo "[1/5] Vérification des dépendances via omarchy-pkg-add..."
  /usr/share/omarchy/bin/omarchy-pkg-add "${DEPS[@]}"
elif command -v pacman >/dev/null 2>&1; then
  echo "[1/5] Vérification des dépendances via pacman..."
  sudo pacman -S --needed --noconfirm "${DEPS[@]}"
else
  echo "Erreur : ce script cible Arch Linux / Omarchy (pacman introuvable)." >&2
  exit 1
fi

# 2. Vérifier l'intégration de Monique (proposé uniquement sur Omarchy)
if ! has_monique; then
  if is_omarchy; then
    echo ""
    echo "[2/5] Monique n'est pas installé sur ce système Omarchy."
    echo "      Monique permet d'appliquer et exporter les profils HDR / ICC dans Hyprland."
    if [[ -t 0 ]] || [[ -e /dev/tty ]]; then
      read -r -p "Installer Monique maintenant (AUR: monique) ? [O/n] " ans </dev/tty || ans="o"
    else
      ans="o"
    fi
    if [[ -z "$ans" || "$ans" =~ ^[OoYy] ]]; then
      if [[ -x /usr/share/omarchy/bin/omarchy-pkg-aur-add ]]; then
        /usr/share/omarchy/bin/omarchy-pkg-aur-add monique
      elif command -v yay >/dev/null 2>&1; then
        yay -S --needed monique
      elif command -v paru >/dev/null 2>&1; then
        paru -S --needed monique
      else
        echo "Aucun assistant AUR trouvé (omarchy-pkg-aur-add / yay / paru)." >&2
      fi
    fi
  else
    echo "[2/5] Monique non détecté (système non-Omarchy : installation non proposée)."
  fi
else
  echo "[2/5] Monique déjà installé et intégré."
fi

# 3. Récupérer les sources ou utiliser le dossier courant
SCRIPT_DIR=""
if [[ -n "${BASH_SOURCE[0]:-}" && -f "${BASH_SOURCE[0]}" ]]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

if [[ -n "$SCRIPT_DIR" && -f "$SCRIPT_DIR/CMakeLists.txt" && -d "$SCRIPT_DIR/src" ]]; then
  SRC_DIR="$SCRIPT_DIR"
  echo "[3/5] Utilisation des sources locales : $SRC_DIR"
else
  echo "[3/5] Téléchargement de TEST_HDR depuis GitHub ($REPO_URL)..."
  mkdir -p "$(dirname "$INSTALL_DIR")"
  if [[ -d "$INSTALL_DIR/.git" ]]; then
    git -C "$INSTALL_DIR" pull --ff-only
  else
    git clone "$REPO_URL" "$INSTALL_DIR"
  fi
  SRC_DIR="$INSTALL_DIR"
fi

# 4. Compilation + tests unitaires
echo "[4/5] Autocompilation et tests..."
chmod +x "$SRC_DIR/bin/build" "$SRC_DIR/bin/run" "$SRC_DIR/bin/install" "$SRC_DIR/bin/monique-set-cm"
"$SRC_DIR/bin/build"

# 5. Installation système (paquet Arch/Omarchy pour lanceur + CLI)
echo "[5/5] Installation du paquet testhdr..."
if command -v makepkg >/dev/null 2>&1 && [[ -f "$SRC_DIR/pkgbuild/PKGBUILD" ]]; then
  (
    cd "$SRC_DIR/pkgbuild"
    rm -rf src pkg ./*.pkg.tar.*
    TESTHDR_SRCROOT="$SRC_DIR" makepkg -fsi --noconfirm
  )
else
  sudo cmake --install "$SRC_DIR/build"
fi

echo ""
echo "TEST_HDR v0.5 installé avec succès !"
echo "  - Terminal : testhdr"
echo "  - Lanceur Omarchy (Super + Space) : TEST_HDR"
echo ""
if [[ -t 0 ]] || [[ -e /dev/tty ]]; then
  read -r -p "Lancer TEST_HDR maintenant ? [O/n] " launch </dev/tty || launch="n"
  if [[ "$launch" =~ ^[OoYy]$ || -z "$launch" ]]; then
    exec testhdr
  fi
fi
