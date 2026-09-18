# The demo, as data: the SQL shown, how long to hold the result, and whether the
# step is meant to fail. A failing statement aborts a batch script, so those are
# captured in their own session and spliced back in order.
STEPS = [
 ("SELECT id, left(body, 44) AS body FROM tickets;", 1.8),

 ("SELECT id, jev_choice(body, MAP{\n"
  "    'refund': 'The customer wants money back',\n"
  "    'bug':    'The customer reports something broken',\n"
  "    'praise': 'The customer is complimenting the product'\n"
  "}) AS intent FROM tickets;", 3.0),

 ("SELECT intent, count(*) AS n FROM (\n"
  "  SELECT jev_choice(body, MAP{'refund':'wants money back',\n"
  "                              'bug':'something broken',\n"
  "                              'praise':'a compliment'}) AS intent\n"
  "  FROM tickets) GROUP BY intent ORDER BY n DESC;", 2.6),

 ("WITH asked AS (\n"
  "  SELECT id, jev_ask(body, {\n"
  "      intent:   MAP{'refund':'wants money back','bug':'something broken',\n"
  "                    'praise':'a compliment'},\n"
  "      severity: ['trivial','minor','normal','serious','critical'],\n"
  "      urgent:   MAP{'true':'needs a reply today','false':'can wait'}\n"
  "  }) AS a FROM tickets)\n"
  "SELECT id, a.intent, round(a.severity,1) AS severity,\n"
  "       round(a.urgent,2) AS urgent, round(a.intent_confidence,2) AS conf\n"
  "FROM asked ORDER BY severity DESC;", 3.4),

 ("SELECT * FROM jev_usage();", 2.4),

 ("SELECT jev_score(body, ['too short']) FROM tickets;", 2.2, True),

 # the same question as three steps ago: every row is served from cache
 ("SELECT intent, count(*) AS n FROM (\n"
  "  SELECT jev_choice(body, MAP{'refund':'wants money back',\n"
  "                              'bug':'something broken',\n"
  "                              'praise':'a compliment'}) AS intent\n"
  "  FROM tickets) GROUP BY intent ORDER BY n DESC;", 2.2),

 ("SELECT * FROM jev_usage();", 3.0),
]
