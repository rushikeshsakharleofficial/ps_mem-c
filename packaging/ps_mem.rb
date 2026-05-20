class PsMem < Formula
  desc "C port of ps_mem with smaps_rollup optimization — 2.2x faster"
  homepage "https://github.com/rushikeshsakharleofficial/ps_mem-c"
  url "https://github.com/rushikeshsakharleofficial/ps_mem-c/archive/refs/tags/v1.0.3.tar.gz"
  version "1.0.3"
  license "LGPL-2.0"

  on_macos do
    on_arm do
      url "https://github.com/rushikeshsakharleofficial/ps_mem-c/releases/download/v1.0.3/ps_mem_1.0.3_darwin_arm64.tar.gz"
    end
    on_intel do
      url "https://github.com/rushikeshsakharleofficial/ps_mem-c/releases/download/v1.0.3/ps_mem_1.0.3_darwin_x86_64.tar.gz"
    end
  end

  on_linux do
    url "https://github.com/rushikeshsakharleofficial/ps_mem-c/archive/refs/tags/v1.0.3.tar.gz"
    depends_on "gcc" => :build
  end

  def install
    if OS.mac?
      bin.install "ps_mem"
    else
      system "gcc", "-O2", "-o", "ps_mem", "ps_mem.c"
      bin.install "ps_mem"
    end
    doc.install "README.md"
  end

  test do
    assert_match "Usage", shell_output("#{bin}/ps_mem -h")
  end
end
