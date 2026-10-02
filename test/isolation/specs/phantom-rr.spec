name phantom-rr
isolation rr

session 1
begin: \begin
first: SELECT ?s WHERE { ?s <http://ex/takes> <http://ex/CS101> }
second: SELECT ?s WHERE { ?s <http://ex/takes> <http://ex/CS101> }
end: \commit

session 2
begin: \begin
ins: INSERT DATA { <http://ex/Dana> <http://ex/takes> <http://ex/CS101> . }
end: \commit

permutation phantom
1.begin
1.first
2.begin
2.ins
2.end
1.second
1.end

expect phantom
1.second contains <http://ex/Dana>
