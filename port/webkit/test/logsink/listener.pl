#!/usr/bin/perl
# A log server for logsink-test (and a title pointed at it with
# /data/ps5webkit/log-sink): accepts one connection after another and
# prints each line with the Mac's time ("HH:MM:SS.mmm line"). A small
# receive buffer (-r <bytes>) makes a stopped listener (kill -STOP) push
# back on the sender sooner.
#   listener.pl [-r 8192] [-a 0.0.0.0] <port>
use strict;
use warnings;
use IO::Socket::INET;
use Socket qw(SOL_SOCKET SO_RCVBUF);
use Time::HiRes qw(time);
my ($rcvbuf, $addr) = (0, '127.0.0.1');
while (@ARGV > 1) {
    my $o = shift;
    if ($o eq '-r') { $rcvbuf = shift } elsif ($o eq '-a') { $addr = shift } else { die "unknown option $o\n" }
}
my $port = shift or die "usage: $0 [-r bytes] [-a addr] port\n";
$| = 1;
my $server = IO::Socket::INET->new(LocalAddr => $addr, LocalPort => $port, Listen => 4, ReuseAddr => 1) or die "listen: $!\n";
setsockopt($server, SOL_SOCKET, SO_RCVBUF, pack('i', $rcvbuf)) if $rcvbuf;
sub stamp { my $t = time; my @l = localtime $t; sprintf '%02d:%02d:%02d.%03d', @l[2, 1, 0], ($t - int $t) * 1000 }
while (my $c = $server->accept) {
    printf "%s ==> connection from %s\n", stamp(), $c->peerhost;
    while (my $line = <$c>) { print stamp(), ' ', $line }
    printf "%s ==> connection closed\n", stamp();
}
