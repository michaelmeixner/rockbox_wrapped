#!/bin/bash
# Lists the Rockbox players to build Wrapped for, with the toolchain each needs.
#
#   ci/list-targets.sh ROCKBOX_TREE [ARCH_LETTERS]
#
# Prints one line per player:  <target> <arch> <full name>
# <arch> is the tools/rockboxdev.sh toolchain letter:
#   a = arm-elf-eabi    i = mipsel-elf    m = m68k-elf
#   x = arm-rockbox-linux-gnueabi (hosted)    y = mipsel-rockbox-linux-gnu (hosted)
# ARCH_LETTERS (e.g. "ai") limits the list to those toolchains.
#
# Players come from Rockbox's own tools/builds.pm (status 2 "unstable" and
# 3 "stable"). The toolchain is worked out from tools/configure, so no
# compiler is needed to run this. Players needing any other toolchain
# (Android, SDL-only handhelds) are left out.
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
