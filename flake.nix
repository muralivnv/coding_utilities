{
  description = "Coding Utilities";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    mio = { url = "https://github.com/vimpunk/mio/archive/8b6b7d878c89e81614d05edca7936de41ccdd2da.tar.gz"; flake = false; };
    pcre2 = { url = "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.tar.gz"; flake = false; };
    treesitter = { url = "https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.25.10.tar.gz"; flake = false; };
    treesitterC = { url = "https://github.com/tree-sitter/tree-sitter-c/archive/refs/tags/v0.24.1.tar.gz"; flake = false; };
    treesitterCpp = { url = "https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz"; flake = false; };
    treesitterPy = { url = "https://github.com/tree-sitter/tree-sitter-python/archive/refs/tags/v0.25.0.tar.gz"; flake = false; };
  };

  outputs = { self, nixpkgs, mio, pcre2, treesitter, treesitterC, treesitterCpp, treesitterPy, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };

      envVarsScript = ''
        export CC="zig cc -s -O3 -target x86_64-linux-musl"
        export CXX="zig c++ -s -O3 -target x86_64-linux-musl"
        export ZIG_GLOBAL_CACHE_DIR="$TMPDIR/zig-cache"
      '';

      depFlags = "-Dmio.tests=OFF -Dmio.installation=OFF -DPCRE2_SUPPORT_JIT=ON -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF";
      
      cmakeCmd = "cmake -B build . -DCMAKE_BUILD_TYPE=Release -GNinja ${depFlags} -DMIO_SOURCE_DIR=${mio} -DPCRE2_SOURCE_DIR=${pcre2} -DTREE_SITTER_SOURCE_DIR=${treesitter} -DTREE_SITTER_C_SOURCE_DIR=${treesitterC} -DTREE_SITTER_CPP_SOURCE_DIR=${treesitterCpp} -DTREE_SITTER_PYTHON_SOURCE_DIR=${treesitterPy}";

    in {
      packages.${system}.default = pkgs.stdenvNoCC.mkDerivation {
        pname = "coding-utilities";
        version = "0.1.0";
        src = self;

        inherit mio pcre2 treesitter treesitterC treesitterCpp treesitterPy;

        nativeBuildInputs = with pkgs; [ cmake ninja bintools zig_0_15 ];
        buildInputs = with pkgs; [ bash ];

        configurePhase = ''
          ${envVarsScript}
          ${cmakeCmd}
        '';

        buildPhase = ''
          cmake --build build
        '';

        installPhase = ''
          mkdir -p $out/bin
          cp build/gai/gai $out/bin/
          cp build/sakura/sakura $out/bin/
        '';
      };

      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ self.packages.${system}.default ];

        buildInputs = with pkgs; [
          git cacert
        ];

        shellHook = ''
          ${envVarsScript}
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
