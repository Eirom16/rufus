# Maintainer: DeepMind Antigravity Pair Programmer
pkgname=rufus-qt
pkgver=1.0.0
pkgrel=1
pkgdesc="The Reliable USB Formatting Utility (Qt6 Port)"
arch=('x86_64')
url="https://github.com/pbatard/rufus"
license=('GPL3')
depends=('qt6-base' 'polkit')
makedepends=('cmake' 'ninja')
source=()

build() {
  cd "$startdir"
  cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build
}

package() {
  install -Dm755 "$startdir/build/src/rufus-qt" "$pkgdir/usr/bin/rufus-qt"

  install -Dm644 "$startdir/rufus-qt.desktop" "$pkgdir/usr/share/applications/rufus-qt.desktop"

  install -Dm644 "$startdir/res/icons/rufus-128.png" "$pkgdir/usr/share/pixmaps/rufus.png"

  install -Dm644 "$startdir/org.rufus.pkexec.policy" "$pkgdir/usr/share/polkit-1/actions/org.rufus.pkexec.policy"
}
