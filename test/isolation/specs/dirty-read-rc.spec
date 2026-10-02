name dirty-read-rc
isolation rc

setup
INSERT DATA { <http://ex/a> <http://ex/type> <http://ex/Student> . }

session 1
begin: \begin
write: INSERT DATA { <http://ex/b> <http://ex/type> <http://ex/Student> . }
end: \commit

session 2
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }
end: \commit

permutation waits
1.begin
2.begin
1.write
2.read
1.end
2.end

expect waits
2.read blocked
1.end unblocks 2.read
2.read contains <http://ex/b>
