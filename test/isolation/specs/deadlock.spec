name deadlock
isolation rc

session 1
begin: \begin
a: INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
b: INSERT DATA { <http://ex/b> <http://ex/p> <http://ex/o> . }
end: \commit

session 2
begin: \begin
b: INSERT DATA { <http://ex/b> <http://ex/p> <http://ex/o> . }
a: INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \abort

permutation cycle
1.begin
2.begin
1.a
2.b
1.b
2.a

expect cycle
1.b blocked
2.a abort
2.a unblocks 1.b
