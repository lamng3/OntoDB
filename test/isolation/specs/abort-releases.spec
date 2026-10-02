name abort-releases
isolation rc

session 1
begin: \begin
ins: INSERT DATA { <http://ex/b> <http://ex/type> <http://ex/Student> . }
end: \abort

session 2
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }
end: \commit

permutation release
1.begin
1.ins
2.begin
2.read
1.end
2.end

expect release
2.read blocked
1.end unblocks 2.read
2.read absent <http://ex/b>
