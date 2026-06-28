#!/usr/bin/env perl
# One-shot codemod for issue #956: naked cell reads + `var x := init`.
#
# Two surface rewrites, applied to the code portion of each line (the
# real `#` comment tail is left untouched so prose is never mangled):
#   1. `@cap`  -> `cap`        drop the read sigil on a mutable cell
#   2. `var x = init` -> `var x := init`   move the declaration to `:=`
#
# The `@` read sigil is always glued to a lowercase identifier
# (`@counter`); the as-pattern is always spaced (`name @ pat`), so the
# `@(?=[a-z_])` match never touches an as-pattern. A `@cap` inside a
# `#{...}` interpolation is part of the code (a real cell read) and is
# rewritten; a `@name` inside a trailing `#` comment is prose and is
# left alone.
#
# Usage: perl tools/upgrade-naked-cell.pl <file.kai> [<file.kai> ...]
# Rewrites each file in place.

use strict;
use warnings;

# Split a line into (code, comment) at the first real `#` — one that is
# not `#{` (interpolation) and not inside a string literal. Returns the
# comment tail including its `#`, or empty string when the line has no
# real comment.
sub split_comment {
    my ($line) = @_;
    my $in_str = 0;      # inside a "..." double-quoted string
    my $i = 0;
    my $n = length($line);
    while ($i < $n) {
        my $c = substr($line, $i, 1);
        if ($in_str) {
            if ($c eq '\\') { $i += 2; next; }   # skip escaped char
            if ($c eq '"') { $in_str = 0; }
            $i++;
            next;
        }
        if ($c eq '"') { $in_str = 1; $i++; next; }
        if ($c eq '#') {
            my $next = ($i + 1 < $n) ? substr($line, $i + 1, 1) : '';
            if ($next eq '{') { $i += 2; next; }   # interpolation, not a comment
            return (substr($line, 0, $i), substr($line, $i));
        }
        $i++;
    }
    return ($line, '');
}

sub rewrite_code {
    my ($code) = @_;
    # Drop the read sigil: `@` glued to a lowercase identifier start.
    $code =~ s/\@(?=[a-z_][A-Za-z0-9_]*)//g;
    # `var name (: Type)? = init` -> `var name (: Type)? := init`.
    # Only the lone `=` that follows the optional annotation, never `==`.
    $code =~ s/^(\s*var\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*:\s*[^=]+?)?)\s=\s/$1 := /;
    return $code;
}

for my $path (@ARGV) {
    open(my $in, '<', $path) or die "cannot read $path: $!";
    my @lines = <$in>;
    close($in);

    my $changed = 0;
    for my $line (@lines) {
        my ($code, $comment) = split_comment($line);
        my $new_code = rewrite_code($code);
        if ($new_code ne $code) {
            $line = $new_code . $comment;
            $changed = 1;
        }
    }

    if ($changed) {
        open(my $out, '>', $path) or die "cannot write $path: $!";
        print $out @lines;
        close($out);
        print "upgraded $path\n";
    }
}
