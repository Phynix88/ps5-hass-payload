# Everything needed to build and test: PS5 payload SDK v0.45 (pinned zip),
# LLVM 21 for the SDK, meson/ninja for microdns, clang + Python for host tests.
{ pkgs ? import <nixpkgs> { } }:
let
  llvm = pkgs.llvmPackages_21;
  sdkZip = pkgs.fetchurl {
    url = "https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip";
    sha256 = "5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c";
  };
  sdk = pkgs.runCommand "ps5-payload-sdk-0.45" { nativeBuildInputs = [ pkgs.unzip ]; } ''
    mkdir -p $out
    unzip -q ${sdkZip} -d $out
  '';
  # The SDK asks `llvm-config --bindir` for clang, ld.lld and llvm-*; the zip
  # loses symlinks, so give it one folder with all of them.
  llvmBin = pkgs.runCommand "ps5-llvm21-bin" { } ''
    mkdir -p $out/bin
    for t in clang clang++ clang-cpp; do ln -s ${llvm.clang-unwrapped}/bin/$t $out/bin/$t; done
    ln -s ${llvm.lld}/bin/ld.lld $out/bin/ld.lld
    for t in llvm-ar llvm-nm llvm-objcopy llvm-ranlib llvm-strip; do
      ln -s ${llvm.llvm}/bin/$t $out/bin/$t
    done
    cat > $out/bin/llvm-config <<WRAP
    #!${pkgs.runtimeShell}
    if [ "\$1" = "--bindir" ]; then echo $out/bin; else exec ${llvm.llvm.dev}/bin/llvm-config "\$@"; fi
    WRAP
    chmod +x $out/bin/llvm-config
  '';
in
pkgs.mkShell {
  packages = [
    pkgs.gnumake pkgs.meson pkgs.ninja pkgs.pkg-config pkgs.git pkgs.curl
    llvm.clang llvm.compiler-rt
    (pkgs.python3.withPackages (ps: [ ps.pytest ps.jsonschema ]))
  ];
  shellHook = ''
    export PS5_PAYLOAD_SDK=${sdk}/ps5-payload-sdk
    export LLVM_CONFIG=${llvmBin}/bin/llvm-config
    export CC=clang
  '';
}
