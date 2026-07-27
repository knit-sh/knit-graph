MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN b.id, count(*) AS n ORDER BY n DESC LIMIT 5
