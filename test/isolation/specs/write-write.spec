name write-write
isolation rc

session 1
begin: \begin
ins: INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \commit

session 2
begin: \begin
ins: INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \commit

permutation same
1.begin
2.begin
1.ins
2.ins
1.end
2.end

expect same
2.ins blocked
1.end unblocks 2.ins
