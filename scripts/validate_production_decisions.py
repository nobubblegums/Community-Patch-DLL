"""Lightweight Task2U schema/source checks and illustrative SQLite fixtures.

Run from any directory with Python 3. No DLL compilation or gameplay execution.
"""
from pathlib import Path
import re
import sqlite3
import subprocess


ROOT = Path(__file__).resolve().parents[1]
DLL = ROOT / 'CvGameCoreDLL_Expansion2'
ENTRY = 'fe6242e'


def entry(name):
    return subprocess.check_output(
        ['git', 'show', ENTRY + ':CvGameCoreDLL_Expansion2/' + name],
        cwd=ROOT, text=True)


trace = (DLL / 'AIUnitDecisionTrace.cpp').read_text()
city = (DLL / 'CvCity.cpp').read_text()
economic = (DLL / 'CvEconomicAI.cpp').read_text()
schemas = {}
for var, cols, table, count in re.findall(
        r'const char\* (\w+)\[\]=\{(.*?)\};\s*Register\("(\w+)",\1,(\d+)\);', trace, re.S):
    columns = re.findall(r'"([IT]):([^"]+)"', cols)
    assert len(columns) == int(count), table
    schemas[table] = columns
assert set(schemas) == {
    'AIProductionDecision', 'AIProductionCandidate', 'AIProductionTerm',
    'AIProductionFilter', 'AIProductionEvent', 'AIPlayerFlavorState', 'AICityFlavorState'}
assert [name for _, name in schemas['AIProductionCandidate']] == [
    'DecisionID', 'Player', 'City', 'BuildableKindName', 'ObjectType',
    'InitialWeight', 'SanityWeight', 'FinalWeight', 'EstimatedTurnsLeft',
    'PoolIndex', 'Selected', 'Reason']
assert [name for _, name in schemas['AIProductionDecision']] == [
    'ObservationID', 'DecisionID', 'Player', 'City', 'FlavorStateID',
    'DecisionType', 'DecisionTrigger', 'SelectionMode', 'WinnerKindName',
    'WinnerObjectType', 'ProductionBeforeObjectType', 'ProductionAfterObjectType',
    'OrderOutcome', 'ProposedAction']
for table, columns in schemas.items():
    needle = {'AIProductionCandidate': 'kCandidateBatch.BeginLogRow()',
              'AIProductionTerm': 'termBatch->BeginLogRow()'}.get(
                  table, 'BeginLogRow("' + table + '")')
    rows = [line for line in trace.splitlines() if needle in line]
    assert rows and all(row.count('.bind(') == len(columns) for row in rows), table
candidate_row = next(line for line in trace.splitlines() if 'kCandidateBatch.BeginLogRow()' in line)
assert '.bind(decisionType.c_str())' not in candidate_row
assert '.bind(selectionMode.c_str())' not in candidate_row
decision_row = next(line for line in trace.splitlines() if 'BeginLogRow("AIProductionDecision")' in line)
assert ('.bind(productionBeforeObjectType.c_str()).bind(productionAfterObjectType.c_str())'
        '.bind(orderOutcome.c_str())') in decision_row
print('PASS seven schemas, exact Decision/Candidate columns and row bindings')

begin = trace.split('void Scope::Begin(', 1)[1].split('Scope::~Scope()', 1)[0]
assert 'traceCity->headOrderQueueNode()' in begin
assert 'productionBeforeObjectType=Symbol(OrderKind(traceHead->eOrderType),traceHead->iData1)' in begin
assert 'productionBeforeObjectType=productionAfterObjectType="UNKNOWN";' in begin
assert 'orderOutcome="NONE";' in begin
result = trace.split('void ProductionOrderResult(', 1)[1].split('void ProductionEvent(', 1)[0]
assert 'origin!=decision' in result and 'player!=c->getOwner()' in result and 'city!=c->GetID()' in result
assert 'decisionType!="ChooseProduction"' in result and '!Enabled()' in result
assert 'const OrderData* head=c->headOrderQueueNode();' in result
assert 'productionAfterObjectType=head ? Symbol(OrderKind(head->eOrderType),head->iData1) : "UNKNOWN";' in result
assert 'orderOutcome=outcome;' in result and 'observedAction=true;' in result
assert 'winnerObjectType' not in result and 'BeginLogRow' not in result
assert trace.count('orderOutcome=') == 2
assert trace.count('productionAfterObjectType=') == 2

push = city.split('void CvCity::pushOrder(', 1)[1].split('void CvCity::popOrder(', 1)[0]
gate = 'if (MOD_AI_UNIT_DECISION_LOGGING && traceDecisionID) '
rejected = gate + 'AIUnitDecisionTrace::ProductionOrderResult(this, traceDecisionID, "REJECTED");'
applied = (gate + 'AIUnitDecisionTrace::ProductionOrderResult(this, traceDecisionID, '
           'tracePreviousOrder<0 ? "STARTED" : tracePreviousOrder==eOrder && '
           'tracePreviousObject==iData1 ? "RESELECTED" : "REPLACED");')
assert push.index('headOrderQueueNode()') < push.index('clearOrderQueue();') < push.index('if (!bValid)')
assert 'if (!bValid)\n\t{\n\t\t' + rejected + '\n\t\treturn;' in push
assert push.index(applied) > max(push.index(native) for native in (
    'm_orderQueue.insertAtEnd(&order)', 'm_orderQueue.insertAtBeginning(&order)',
    'startHeadOrder();', 'SetSpecificCityInfoDirty'))
assert city.count('AIUnitDecisionTrace::ProductionOrderResult(') == 2
for path in DLL.rglob('*.cpp'):
    source = path.read_text(encoding='utf-8-sig')
    assert 'PRODUCTION_ORDER_' not in source, path
    if path.name != 'CvCity.cpp':
        assert 'AIUnitDecisionTrace::ProductionOrderResult(' not in source, path

# Recover only the two changed diagnostic calls; all remaining CvCity bytes,
# including early returns, arithmetic, braces and queue operations, must match.
previous = entry('CvCity.cpp')
old_calls = [line.strip() for line in previous.splitlines() if '"PRODUCTION_ORDER_' in line]
assert len(old_calls) == 2
assert city.replace(rejected, old_calls[0]).replace(applied, old_calls[1]) == previous
for name in ('CvCityStrategyAI.cpp', 'CvEconomicAI.cpp', 'CvUnitProductionAI.cpp',
             'CvReligionClasses.cpp', 'SqliteLogger.cpp', 'SqliteLogger.h'):
    assert (DLL / name).read_text() == entry(name), name
print('PASS native queue before/after boundaries, rejection, gates and unchanged gameplay source')

event_body = trace.split('void ProductionEvent(', 1)[1].split('Action::Action()', 1)[0]
old_event = entry('AIUnitDecisionTrace.cpp').split('void ProductionEvent(', 1)[1].split('Action::Action()', 1)[0]
assert event_body == old_event  # Includes DecisionID=0 direct events.
assert trace.split('Action::Action()', 1)[1].split('bool Configured()', 1)[0] == entry(
    'AIUnitDecisionTrace.cpp').split('Action::Action()', 1)[1].split('bool Configured()', 1)[0]
for event in ('BUILDING_INVESTMENT', 'BUILDING_PURCHASE', 'UNIT_INVESTMENT', 'UNIT_PURCHASE'):
    assert '"' + event + '"' in trace
assert '"HURRY_PLAYER_CANDIDATE_CONSIDERED"' in economic
assert '"PRODUCTION_COMPLETED"' in city
investment = economic.split('pSelectedCity->SetBuildingInvestment(eBuildingClass, true);', 1)[1]
assert 'if (MOD_AI_UNIT_DECISION_LOGGING && pSelectedCity->IsBuildingInvestment(eBuildingClass)) traceAction.Success();' in investment
print('PASS retained Hurry consideration, action/investment checks, completion and direct events')

# Fixtures illustrate independent investment and queue facts; these are not game observations.
db = sqlite3.connect(':memory:')
for table, columns in schemas.items():
    db.execute('CREATE TABLE ' + table + '(GameId INTEGER, Turn INTEGER,' + ','.join(
        name + (' INTEGER' if kind == 'I' else ' TEXT') for kind, name in columns) + ')')


def insert(table, **values):
    names = ['GameId', 'Turn'] + [name for _, name in schemas[table]]
    defaults = dict(GameId=1, Turn=100, Player='Rome', City='Rome')
    defaults.update(values)
    db.execute('INSERT INTO ' + table + ' VALUES (' + ','.join('?' for _ in names) + ')',
               [defaults.get(name) for name in names])


for did, outcome, before, after in (
        (1, 'STARTED', 'UNKNOWN', 'BUILDING_WALLS'),
        (2, 'RESELECTED', 'BUILDING_WALLS', 'BUILDING_WALLS'),
        (3, 'REPLACED', 'BUILDING_LIBRARY', 'BUILDING_WALLS'),
        (4, 'REJECTED', 'BUILDING_LIBRARY', 'UNKNOWN'),
        (5, 'REJECTED', 'BUILDING_LIBRARY', 'BUILDING_LIBRARY'),
        (6, 'NONE', 'BUILDING_LIBRARY', 'UNKNOWN')):
    insert('AIProductionDecision', DecisionID=did, DecisionType='ChooseHurry' if did == 6 else 'ChooseProduction',
           WinnerObjectType='BUILDING_WALLS', ProductionBeforeObjectType=before,
           ProductionAfterObjectType=after, OrderOutcome=outcome, ProposedAction='BUILDING_INVESTMENT' if did == 6 else 'NONE')
    insert('AIProductionCandidate', DecisionID=did, BuildableKindName='BUILDING',
           ObjectType='BUILDING_WALLS', InitialWeight=100, SanityWeight=100, FinalWeight=80,
           EstimatedTurnsLeft=0, PoolIndex=0, Selected=1, Reason='')
insert('AIProductionEvent', DecisionID=6, EventType='HURRY_PLAYER_CANDIDATE_CONSIDERED',
       ObjectType='BUILDING_WALLS', Currency='GOLD', Result='EVALUATED')
insert('AIProductionEvent', DecisionID=6, EventType='BUILDING_INVESTMENT',
       ObjectType='BUILDING_WALLS', Currency='GOLD', Result='APPLIED')
insert('AIProductionEvent', DecisionID=0, EventType='UNIT_PURCHASE',
       ObjectType='UNIT_DIPLOMAT', Currency='GOLD', Result='APPLIED')
assert db.execute("SELECT ProductionBeforeObjectType,ProductionAfterObjectType,OrderOutcome FROM AIProductionDecision WHERE DecisionID=6").fetchone() == ('BUILDING_LIBRARY', 'UNKNOWN', 'NONE')
assert db.execute("SELECT DecisionID FROM AIProductionEvent WHERE EventType='BUILDING_INVESTMENT' AND Result='APPLIED' AND ObjectType='BUILDING_WALLS'").fetchall() == [(6,)]
assert db.execute("SELECT count(*) FROM AIProductionEvent WHERE DecisionID=0").fetchone() == (1,)
assert db.execute("SELECT count(*) FROM AIProductionEvent WHERE EventType LIKE 'PRODUCTION_ORDER_%'").fetchone() == (0,)
assert db.execute("SELECT count(*) FROM AIProductionCandidate JOIN AIProductionDecision USING(GameId,DecisionID) WHERE Selected=1 AND ObjectType=WinnerObjectType").fetchone() == (6,)
assert db.execute("SELECT count(*) FROM AIProductionDecision WHERE ProductionAfterObjectType='BUILDING_WALLS' AND OrderOutcome IN ('STARTED','RESELECTED','REPLACED')").fetchone() == (3,)
print('PASS SQLite fixtures: outcomes, zero-turn candidates, linked/direct events and investment independent of queue')
print('STATIC_VALIDATED; NOT COMPILED / NOT RUNTIME TESTED')
