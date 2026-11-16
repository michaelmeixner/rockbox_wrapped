#!/bin/bash
# ci/list-targets.sh ROCKBOX_TREE [ARCH_LETTERS]
# Prints "<target> <arch> <name>" for each usable player in tools/builds.pm.
# arch is the rockboxdev.sh letter, read from tools/configure: a=ARM, i=MIPS,
# m=m68k, x/y=hosted Linux ARM/MIPS. Other toolchains are skipped.
set -euo pipefail
tree="${1:?usage: list-targets.sh ROCKBOX_TREE [ARCH_LETTERS]}"
want="${2:-aimxy}"
cd "$tree"
WANT="$want" perl -e '
  require "./tools/builds.pm";
  open my $f, "<", "tools/configure" or die "no tools/configure\n";
  my $src = do { local $/; <$f> }; close $f;
  my %fn;                                  # shell functions in configure
  while ($src =~ /^(\w+)\s*\(\)\s*\{(.*?)^\}/msg) { $fn{$1} = $2; }
  sub prefix_of {                          # follow helper calls to prefixtools
    my ($n, $d) = @_;
    return "" if $d > 4 or !exists $fn{$n};
    return $1 if $fn{$n} =~ /prefixtools\s+"?([\w.\-]+)"?/;
    for my $call ($fn{$n} =~ /^\s*(\w+)\s*$/mg) {
      my $p = prefix_of($call, $d + 1); return $p if $p ne "";
    }
    return "";
  }
  my %arch = ("arm-elf-eabi-" => "a", "mipsel-elf-" => "i", "m68k-elf-" => "m",
              "arm-rockbox-linux-gnueabi-" => "x",
              "mipsel-rockbox-linux-gnu-" => "y");
  for my $t (sort grep { $builds{$_}{status} >= 2 } keys %builds) {
    my ($blk) = $src =~ /^[^\n]*\|\Q$t\E[|)][^\n]*\n(.*?);;/ms or next;
    my ($cc) = $blk =~ /^\s*(\w+cc)\s*$/m or next;
    my $a = $arch{prefix_of($cc, 0)} or next;
    next if index($ENV{WANT}, $a) < 0;
    print "$t $a $builds{$t}{name}\n";
  }
'
