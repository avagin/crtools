import sys

class Process:
	def __init__(self, pid, ppid, sid):
		global root
		self.pid	= int(pid)
		self.sid	= int(sid)
		self.ppid	= int(ppid)
		self.born_sid	= None
		self.born_sids	= set()
		self.children	= []
		self.new_parent = None
		self.aval_sids	= set()
		self.exc_sids	= set()

	def leader(self):
		return self.pid == self.sid

	def set_born_sids(self):
		if not self.leader():
			self.born_sid = self.sid
			self.born_sids.add(self.sid)
			return

		map(lambda a: self.born_sids.update(a.born_sids), self.children)

	def set_exc_sids(self):
		if self.parent:
			self.exc_sids = set.union(self.parent.exc_sids)
			self.exc_sids.add(self.parent.sid)

	def __repr__(self):
		return repr((self.pid, self.ppid, self.sid))

	def __str__(self):
		return "%d,%d,%d %s %s %s" % (self.pid, self.ppid, self.sid, repr(self.born_sids), repr(self.aval_sids), repr(self.exc_sids))

global root, sids
root = None
processes = []
sids = set()

for l in sys.stdin:
	if l[0] == '#':
		continue
	pid, ppid, sid = l.split()
	p = Process(pid, ppid, sid)
	processes.append(p)
	sids.add(p.sid)

print sids

for p in processes:
	p.parent = filter(lambda a: a.pid == p.ppid, processes)
	if len(p.parent) == 0:
		if root:
			raise Exception(p)
		root = p
		p.parent = None
	else:
		assert(len(p.parent) == 1)
		p.parent = p.parent[0]
		p.parent.children.append(p)

p = root
while True:
	p.set_exc_sids()
	if p.children:
		p = p.children[0]
		continue
	while True:
		p.set_born_sids()
		print p

		if not p.parent:
			break

		i = p.parent.children.index(p)
		if len(p.parent.children) - 1 != i:
			p = p.parent.children[i + 1]
			break

		p = p.parent

	if not p.parent:
		break

pr_seqs = [root]
root.aval_sids = sids
print "----"
print sids
for p in processes:
	print p
	if p == root:
		continue
	if not p.leader():
		continue
	for i in xrange(len(pr_seqs)):
		pp = pr_seqs[i];
		if not p.born_sids.issubset(pp.aval_sids):
			break
	else:
		i += 1

	assert(i != 0)

	pp = pr_seqs[i -1]
	p.aval_sids = pp.aval_sids - set([p.sid])
	pr_seqs.insert(i, p)

	for c in pr_seqs[i:]:
		c.aval_sids -= p.exc_sids
		c.aval_sids -= set([p.sid])

print "----"
print pr_seqs[0]
for i in xrange(1, len(pr_seqs)):
	p = pr_seqs[i]
	pp = pr_seqs[i - 1]
	print p
	if p.aval_sids.issubset(pp.aval_sids):
		continue
	raise Exception(p, pp)

print "----"
for p in pr_seqs:
	print p
