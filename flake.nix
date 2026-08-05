{
  description = "Koi";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    pcre2 = { url = "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.tar.gz"; flake = false; };
    treesitter = { url = "https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.26.10.tar.gz"; flake = false; };
    treesitterC = { url = "https://github.com/tree-sitter/tree-sitter-c/archive/refs/tags/v0.24.2.tar.gz"; flake = false; };
    treesitterCpp = { url = "https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz"; flake = false; };
    treesitterPy = { url = "https://github.com/tree-sitter/tree-sitter-python/archive/refs/tags/v0.25.0.tar.gz"; flake = false; };
    termbox = { url = "https://github.com/termbox/termbox2/archive/605398fa79108412976191e062ea14bd4bd30213.tar.gz"; flake = false; };
  };

  outputs = { self, nixpkgs, pcre2, treesitter, treesitterC, treesitterCpp, treesitterPy, termbox, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };

      # termbox2 has no inline (non-fullscreen) mode. The patch adds one: no
      # alternate screen, a row offset applied where cell rows become terminal
      # rows, and no clear-screen on resize or exit.
      termboxPatched = pkgs.runCommand "termbox2-inline" { } ''
        cp -r ${termbox} $out
        chmod -R u+w $out
        patch -p1 --batch -d $out < ${./external/patches/termbox2-inline.patch}
      '';

      depFlags = "-DPCRE2_SUPPORT_JIT=ON -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF";
      cmakeCmd = "cmake -B build . -DCMAKE_BUILD_TYPE=Release -GNinja ${depFlags} -DPCRE2_SOURCE_DIR=${pcre2} -DTREE_SITTER_SOURCE_DIR=${treesitter} -DTREE_SITTER_C_SOURCE_DIR=${treesitterC} -DTREE_SITTER_CPP_SOURCE_DIR=${treesitterCpp} -DTREE_SITTER_PYTHON_SOURCE_DIR=${treesitterPy} -DTERMBOX_SOURCE_DIR=${termboxPatched}";

    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "koi";
        version = "0.1.0";
        src = self;

        inherit pcre2 treesitter treesitterC treesitterCpp treesitterPy;

        nativeBuildInputs = with pkgs; [ cmake ninja sqlite ];
        buildInputs = with pkgs; [ bash libunistring ];

        configurePhase = ''
          ${cmakeCmd}
        '';

        buildPhase = ''
          cmake --build build
        '';

        doCheck = true;
        checkPhase = ''
          runHook preCheck
          ctest --test-dir build --output-on-failure
          runHook postCheck
        '';

        installPhase = ''
          mkdir -p $out/bin
          cp build/gai/gai $out/bin/
          cp build/sakura/sakura $out/bin/
          cp build/ghatothkacha/ghatothkacha $out/bin/
          cp build/tooey/tooey $out/bin
        '';
      };

      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ self.packages.${system}.default ];

        buildInputs = with pkgs; [
          git cacert
        ];

        shellHook = ''
          export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
          export CURL_CA_BUNDLE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
    
          cmake-configure() {
            ${cmakeCmd} "$@"
          }

          echo "Dev shell ready. Run 'cmake-configure' to configure."
        '';
      };
    };
}   
