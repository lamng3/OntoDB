name phantom-ser
isolation ser

session 1
begin: \begin
first: SELECT ?s WHERE { ?s <http://ex/takes> <http://ex/CS101> }
end: \commit

session 2
begin: \begin
ins: INSERT DATA { <http://ex/Dana> <http://ex/takes> <http://ex/CS101> . }
end: \commit

permutation gap
1.begin
1.first
2.begin
2.ins
1.end
2.end

expect gap
2.ins blocked
1.end unblocks 2.ins
