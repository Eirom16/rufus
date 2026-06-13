# Maintainer: DeepMind Antigravity Pair Programmer
pkgname=rufus-linux
pkgver=1.0.0
pkgrel=1
pkgdesc="The Reliable USB Formatting Utility for Arch Linux (Native GTK3 Port)"
arch=('x86_64')
url="https://github.com/pbatard/rufus"
license=('GPL3')
depends=('gtk3' 'openssl' 'polkit')
makedepends=('pkg-config' 'autoconf' 'automake' 'make')
source=()

build() {
  cd "$startdir"
  # Clean previous builds to guarantee fresh packaging
  make clean 2>/dev/null || true
  ./bootstrap.sh
  ./configure --prefix=/usr
  make
}

package() {
  # Install the compiled ELF widescreen binary to system bins
  install -Dm755 "$startdir/src/rufus-linux" "$pkgdir/usr/bin/rufus-linux"

  # Install the premium desktop launcher shortcut
  install -Dm644 "$startdir/rufus-linux.desktop" "$pkgdir/usr/share/applications/rufus-linux.desktop"

  # Install the official high-fidelity icon globally
  install -Dm644 "$startdir/res/icons/rufus-128.png" "$pkgdir/usr/share/pixmaps/rufus.png"

  # Install the Polkit policy file for graphical pkexec authentication
  install -Dm644 "$startdir/org.rufus.pkexec.policy" "$pkgdir/usr/share/polkit-1/actions/org.rufus.pkexec.policy"
}
