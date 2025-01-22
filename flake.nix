{
  description = "A very basic flake";

  outputs = { self, nixpkgs }: let
    pkgs = nixpkgs.legacyPackages.x86_64-linux;
  in {

    packages.x86_64-linux.firmware = let
      pico-sdk-with-submodules = pkgs.pico-sdk.override {
        withSubmodules = true;
      };
    in 
    pkgs.stdenv.mkDerivation {
      name = "blaustahl-firmware";
      src = ./firmware/blaustahl;
      nativeBuildInputs = with pkgs; [
        cmake
        gcc-arm-embedded
        gnumake
        picotool
        python3
      ];
      buildInputs = [
        pico-sdk-with-submodules
      ];
      PICO_SDK_PATH="${pico-sdk-with-submodules}/lib/pico-sdk";
      cmakeFlags = [
        "-DCMAKE_CXX_COMPILER=${pkgs.gcc-arm-embedded}/bin/arm-none-eabi-g++"
        "-DCMAKE_C_COMPILER=${pkgs.gcc-arm-embedded}/bin/arm-none-eabi-gcc"
        "-DCMAKE_MAKE_PROGRAM=${pkgs.gnumake}/bin/make"
      ];
      installPhase = ''
        install -Dm444 blaustahl.uf2 $out/lib/firmware/blaustahl.uf2
        install -Dm444 blaustahl_cdconly.uf2 $out/lib/firmware/blaustahl_cdconly.uf2
      '';
      dontFixup = true;
    };

    packages.x86_64-linux.bs = pkgs.stdenv.mkDerivation {
      name = "bs";
      src = ./sw;

      makeFlags = [
        "CFLAGS=-I${pkgs.libusb1.dev}/include"
        "LDFLAGS=-L${pkgs.libusb1.out}/lib"
        "bindir=/bin/bs"
        "DESTDIR=$(out)"
      ];
    };

    packages.x86_64-linux.srwp = pkgs.writers.writePython3Bin "srwp" {
      libraries = [ pkgs.python3Packages.pyserial ];
      doCheck = false;
    } (builtins.readFile ./sw/srwp.py);

    packages.x86_64-linux.default = self.packages.x86_64-linux.bs;

  };
}
