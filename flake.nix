{
  description = "Koi";

  inputs = {
    nixpkgs.url  = "github:NixOS/nixpkgs/0ad6f47ea4fe188f4bc8f0380f93ae8523337c6c";  # nixos-26.05, 2026-07-07
    pcre2        = { url = "git+https://github.com/PCRE2Project/pcre2?ref=refs/tags/pcre2-10.47&submodules=1"           ; flake = false; };
    termbox      = { url = "https://github.com/termbox/termbox2/archive/605398fa79108412976191e062ea14bd4bd30213.tar.gz"; flake = false; };
    tomlplusplus = { url = "https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz"                     ; flake = false; };

    treesitter    = { url = "https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.26.12.tar.gz"             ; flake = false; };
    ts-bash       = { url = "github:tree-sitter/tree-sitter-bash/a06c2e4415e9bc0346c6b86d401879ffb44058f7"             ; flake = false; };
    ts-c          = { url = "github:tree-sitter/tree-sitter-c/b780e47fc780ddc8da13afa35a3f4ed5c157823d"                ; flake = false; };
    ts-cmake      = { url = "github:uyha/tree-sitter-cmake/ca627bb5828616b6246aafdc3c3222789e728e37"                   ; flake = false; };
    ts-cpp        = { url = "github:tree-sitter/tree-sitter-cpp/8b5b49eb196bec7040441bee33b2c9a4838d6967"              ; flake = false; };
    ts-css        = { url = "github:tree-sitter/tree-sitter-css/dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f"              ; flake = false; };
    ts-dart       = { url = "github:UserNobody14/tree-sitter-dart/be07cf7118d3dba06236a3f19541685a68209934"            ; flake = false; };
    ts-diff       = { url = "github:tree-sitter-grammars/tree-sitter-diff/0f8fe525b2ff5fbd7f21d0dcfb756fbbe30ca844"    ; flake = false; };
    ts-go         = { url = "github:tree-sitter/tree-sitter-go/2346a3ab1bb3857b48b29d779a1ef9799a248cd7"               ; flake = false; };
    ts-html       = { url = "github:tree-sitter/tree-sitter-html/73a3947324f6efddf9e17c0ea58d454843590cc0"             ; flake = false; };
    ts-javascript = { url = "github:tree-sitter/tree-sitter-javascript/58404d8cf191d69f2674a8fd507bd5776f46cb11"       ; flake = false; };
    ts-json       = { url = "github:tree-sitter/tree-sitter-json/001c28d7a29832b06b0e831ec77845553c89b56d"             ; flake = false; };
    ts-make       = { url = "github:tree-sitter-grammars/tree-sitter-make/70613f3d812cbabbd7f38d104d60a409c4008b43"    ; flake = false; };
    ts-markdown   = { url = "github:tree-sitter-grammars/tree-sitter-markdown/a0a00f817d02412bd92c54d316f164d827b57b5c"; flake = false; };
    ts-nix        = { url = "github:nix-community/tree-sitter-nix/3d0173d903e630b6e14d17f1cf79488791379ded"            ; flake = false; };
    ts-python     = { url = "github:tree-sitter/tree-sitter-python/26855eabccb19c6abf499fbc5b8dc7cc9ab8bc64"           ; flake = false; };
    ts-rust       = { url = "github:tree-sitter/tree-sitter-rust/77a3747266f4d621d0757825e6b11edcbf991ca5"             ; flake = false; };
    ts-toml       = { url = "github:tree-sitter-grammars/tree-sitter-toml/64b56832c2cffe41758f28e05c756a3a98d16f41"    ; flake = false; };
    ts-typescript = { url = "github:tree-sitter/tree-sitter-typescript/75b3874edb2dc714fb1fd77a32013d0f8699989f"       ; flake = false; };
    ts-yaml       = { url = "github:tree-sitter-grammars/tree-sitter-yaml/a1c4812a73ec5e089de8e441fdea3a921e8d5079"    ; flake = false; };
  };

  outputs = { self, nixpkgs, ... }@inputs:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      inherit (pkgs.lib) concatStringsSep mapAttrsToList optionalString;

      termboxPatched = pkgs.runCommand "termbox2-inline" { } ''
        cp -r ${inputs.termbox} $out
        chmod -R u+w $out
        patch -p1 --batch -d $out < ${./external/patches/termbox2-inline.patch}
      '';

      grammars = {
        bash       = { src = inputs.ts-bash; };
        c          = { src = inputs.ts-c; };
        cmake      = { src = inputs.ts-cmake; };
        cpp        = { src = inputs.ts-cpp; };
        css        = { src = inputs.ts-css; };
        dart       = { src = inputs.ts-dart; };
        diff       = { src = inputs.ts-diff; };
        go         = { src = inputs.ts-go; };
        html       = { src = inputs.ts-html; };
        javascript = { src = inputs.ts-javascript; };
        json       = { src = inputs.ts-json; };
        make       = { src = inputs.ts-make; };
        markdown   = { src = inputs.ts-markdown; sub = "tree-sitter-markdown"; };
        markdown_inline = { src = inputs.ts-markdown; sub = "tree-sitter-markdown-inline"; };
        nix        = { src = inputs.ts-nix; };
        python     = { src = inputs.ts-python; };
        rust       = { src = inputs.ts-rust; };
        toml       = { src = inputs.ts-toml; };
        tsx        = { src = inputs.ts-typescript; sub = "tsx"; };
        typescript = { src = inputs.ts-typescript; sub = "typescript"; };
        yaml       = { src = inputs.ts-yaml; };
      };

      grammarPaths = concatStringsSep ";"
        (mapAttrsToList (name: g: "${name}=${g.src}${optionalString (g ? sub) "/${g.sub}"}") grammars);

      cmakeFlags = [
        "-DCMAKE_BUILD_TYPE=Release"
        "-GNinja"
        "-DPCRE2_SUPPORT_JIT=ON"
        "-DPCRE2_BUILD_PCRE2GREP=OFF"
        "-DPCRE2_BUILD_TESTS=OFF"
        "-DPCRE2_SOURCE_DIR=${inputs.pcre2}"
        "-DTERMBOX_SOURCE_DIR=${termboxPatched}"
        "-DTOMLPLUSPLUS_SOURCE_DIR=${inputs.tomlplusplus}"
        "-DTREE_SITTER_SOURCE_DIR=${inputs.treesitter}"
        "-DTREE_SITTER_GRAMMARS='${grammarPaths}'"
      ];
      cmakeCmd = "cmake -B build . ${concatStringsSep " " cmakeFlags}";

    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "koi";
        version = "dude.just.don't";
        src = self;

        nativeBuildInputs = with pkgs; [ cmake ninja sqlite patchelf ];
        buildInputs = with pkgs; [ bash libunistring sqlite ];

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
          mkdir -p $out/bin $out/lib $out/share/koi
          for b in gai ghatothkacha tooey koi; do
            cp build/*/$b $out/bin/
          done
          cp build/external/tree-sitter/*.so* $out/lib/
          ln -s $out/lib $out/share/koi/grammars
          cp -r koi/queries koi/themes $out/share/koi/
          cp koi/config.reference.toml $out/share/koi/
          for b in gai ghatothkacha tooey koi; do
            patchelf --set-rpath "$out/lib:${pkgs.libunistring}/lib:${pkgs.stdenv.cc.cc.lib}/lib" $out/bin/$b
          done
        '';
      };

      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ self.packages.${system}.default ];

        buildInputs = with pkgs; [
          git cacert patchelf
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
