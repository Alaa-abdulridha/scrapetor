require "rake/testtask"
require "fileutils"

Rake::TestTask.new(:test) do |t|
  t.libs << "test"
  t.libs << "lib"
  t.test_files = FileList["test/**/test_*.rb"]
end

task default: %i[compile test]

EXT_DIR = File.expand_path("ext/scrapetor/native", __dir__)
LIB_DIR = File.expand_path("lib/scrapetor", __dir__)
DLEXT   = RbConfig::CONFIG["DLEXT"]

desc "Compile native streaming extension (#{DLEXT})"
task :compile do
  Dir.chdir(EXT_DIR) do
    FileUtils.rm_f "Makefile"
    sh "ruby extconf.rb"
    sh "make"
    binary_name = "scrapetor_native.#{DLEXT}"
    # mkmf with target_prefix puts the binary in ./scrapetor/<binary>
    candidates = [binary_name, File.join("scrapetor", binary_name)]
    src = candidates.find { |f| File.exist?(f) }
    abort "build produced no binary; looked in: #{candidates.inspect}" unless src
    FileUtils.cp(src, File.join(LIB_DIR, binary_name))
    puts "installed #{File.join(LIB_DIR, binary_name)}"
  end
end

desc "Clean compiled artifacts (never deletes .c sources)"
task :clean do
  Dir.chdir(EXT_DIR) do
    sh "make clean" if File.exist?("Makefile")
    FileUtils.rm_rf "scrapetor"
    %w[
      scrapetor_native.o scrapetor_native.bundle scrapetor_native.so
      scrapetor_dom.o
      Makefile mkmf.log
    ].each { |f| FileUtils.rm_f f }
  end
  FileUtils.rm_f File.join(LIB_DIR, "scrapetor_native.#{DLEXT}")
end

desc "Run benchmarks against Nokogiri and Nokolexbor"
task :bench => :compile do
  ruby "-Ilib", "benchmark/parse_extract.rb"
end
