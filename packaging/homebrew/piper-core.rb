class Piper < Formula
  desc "Python 3.14 implementation with a native compiler and terminal-first dialect"
  homepage "https://github.com/Hexadecimall/piper"
  url "https://github.com/Hexadecimall/piper/archive/refs/tags/v0.1.4.tar.gz"
  sha256 "6a24eb82c9a1b298bb84c6027c1fc67eac95d55a32929ca12cab0e54fd5482a8"
  license "MIT"

  depends_on "python@3.14" => :build
  depends_on "rust" => :build
  depends_on "lld"
  depends_on "llvm"

  deny_network_access!

  def fetch
    super
    system "cargo", "fetch", "--locked", "--target", "host-tuple"
  end

  def install
    llvm = formula_opt_prefix("llvm")
    lld = formula_opt_prefix("lld")
    architecture = Hardware::CPU.arm? ? "aarch64" : "x86_64"
    platform = OS.mac? ? "macos" : "linux-gnu"

    ENV["PIPER_LLVM_PREFIX"] = llvm
    ENV["PIPER_LLD_PREFIX"] = lld
    ENV["PIPER_LLVM_LINK"] = "dynamic"
    ENV["PIPER_LLD_LINK"] = "dynamic"
    ENV["PIPER_LLVM_RUNTIME_DIR"] = llvm/"lib"
    ENV["PIPER_CLANG"] = llvm/"bin/clang"
    ENV["PIPER_AR"] = llvm/"bin/llvm-ar"
    ENV["PIPER_CROSS_TARGETS"] = "#{architecture}-#{platform}"
    ENV["PIPER_DISABLE_SELF_UPDATE"] = "1"

    system "cargo", "install", *std_cargo_args
  end

  test do
    assert_equal "piper #{version}", shell_output("#{bin}/piper --version").strip
    (testpath/"hello.py").write "print('piper')\n"
    system bin/"piper", "compile", testpath/"hello.py", "-o", testpath/"hello"
    assert_equal "piper", shell_output(testpath/"hello").strip
  end
end
