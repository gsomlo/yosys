/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2019-2020  whitequark <whitequark@whitequark.org>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/utils.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// [[CITE]]
// Peter Eades; Xuemin Lin; W. F. Smyth, "A Fast Effective Heuristic For The Feedback Arc Set Problem"
// Information Processing Letters, Vol. 47, pp 319-323, 1993
// https://pdfs.semanticscholar.org/c7ed/d9acce96ca357876540e19664eb9d976637f.pdf

// A topological sort (on a cell/wire graph) is always possible in a fully flattened RTLIL design without
// processes or logic loops where every wire has a single driver. Logic loops are illegal in RTLIL and wires
// with multiple drivers can be split by the `splitnets` pass; however, interdependencies between processes
// or module instances can create strongly connected components without introducing evaluation nondeterminism.
// We wish to support designs with such benign SCCs (as well as designs with multiple drivers per wire), so
// we sort the graph in a way that minimizes feedback arcs. If there are no feedback arcs in the sorted graph,
// then a more efficient evaluation method is possible, since eval() will always immediately converge.
template<class T>
struct Scheduler {
	struct Vertex {
		T *data;
		Vertex *prev, *next;
		pool<Vertex*, hash_ptr_ops> preds, succs;

		Vertex() : data(NULL), prev(this), next(this) {}
		Vertex(T *data) : data(data), prev(NULL), next(NULL) {}

		bool empty() const
		{
			log_assert(data == NULL);
			if (next == this) {
				log_assert(prev == next);
				return true;
			}
			return false;
		}

		void link(Vertex *list)
		{
			log_assert(prev == NULL && next == NULL);
			next = list;
			prev = list->prev;
			list->prev->next = this;
			list->prev = this;
		}

		void unlink()
		{
			log_assert(prev->next == this && next->prev == this);
			prev->next = next;
			next->prev = prev;
			next = prev = NULL;
		}

		int delta() const
		{
			return succs.size() - preds.size();
		}
	};

	std::vector<Vertex*> vertices;
	Vertex *sources = new Vertex;
	Vertex *sinks = new Vertex;
	dict<int, Vertex*> bins;

	~Scheduler()
	{
		delete sources;
		delete sinks;
		for (auto bin : bins)
			delete bin.second;
		for (auto vertex : vertices)
			delete vertex;
	}

	Vertex *add(T *data)
	{
		Vertex *vertex = new Vertex(data);
		vertices.push_back(vertex);
		return vertex;
	}

	void relink(Vertex *vertex)
	{
		if (vertex->succs.empty())
			vertex->link(sinks);
		else if (vertex->preds.empty())
			vertex->link(sources);
		else {
			int delta = vertex->delta();
			if (!bins.count(delta))
				bins[delta] = new Vertex;
			vertex->link(bins[delta]);
		}
	}

	Vertex *remove(Vertex *vertex)
	{
		vertex->unlink();
		for (auto pred : vertex->preds) {
			if (pred == vertex)
				continue;
			log_assert(pred->succs[vertex]);
			pred->unlink();
			pred->succs.erase(vertex);
			relink(pred);
		}
		for (auto succ : vertex->succs) {
			if (succ == vertex)
				continue;
			log_assert(succ->preds[vertex]);
			succ->unlink();
			succ->preds.erase(vertex);
			relink(succ);
		}
		vertex->preds.clear();
		vertex->succs.clear();
		return vertex;
	}

	std::vector<Vertex*> schedule()
	{
		std::vector<Vertex*> s1, s2r;
		for (auto vertex : vertices)
			relink(vertex);
		bool bins_empty = false;
		while (!(sinks->empty() && sources->empty() && bins_empty)) {
			while (!sinks->empty())
				s2r.push_back(remove(sinks->next));
			while (!sources->empty())
				s1.push_back(remove(sources->next));
			// Choosing u in this implementation isn't O(1), but the paper handwaves which data structure they suggest
			// using to get O(1) relinking *and* find-max-key ("it is clear"... no it isn't), so this code uses a very
			// naive implementation of find-max-key.
			bins_empty = true;
			bins.template sort<std::greater<int>>();
			for (auto bin : bins) {
				if (!bin.second->empty()) {
					bins_empty = false;
					s1.push_back(remove(bin.second->next));
					break;
				}
			}
		}
		s1.insert(s1.end(), s2r.rbegin(), s2r.rend());
		return s1;
	}
};

static bool is_unary_cell(RTLIL::IdString type)
{
	return type.in(
		ID($not), ID($logic_not), ID($reduce_and), ID($reduce_or), ID($reduce_xor), ID($reduce_xnor), ID($reduce_bool),
		ID($pos), ID($neg));
}

static bool is_binary_cell(RTLIL::IdString type)
{
	return type.in(
		ID($and), ID($or), ID($xor), ID($xnor), ID($logic_and), ID($logic_or),
		ID($shl), ID($sshl), ID($shr), ID($sshr), ID($shift), ID($shiftx),
		ID($eq), ID($ne), ID($eqx), ID($nex), ID($gt), ID($ge), ID($lt), ID($le),
		ID($add), ID($sub), ID($mul), ID($div), ID($mod));
}

static bool is_elidable_cell(RTLIL::IdString type)
{
	return is_unary_cell(type) || is_binary_cell(type) || type.in(
		ID($mux), ID($concat), ID($slice));
}

static bool is_sync_ff_cell(RTLIL::IdString type)
{
	return type.in(
		ID($dff), ID($dffe));
}

static bool is_ff_cell(RTLIL::IdString type)
{
	return is_sync_ff_cell(type) || type.in(
		ID($adff), ID($dffsr), ID($dlatch), ID($dlatchsr), ID($sr));
}

static bool is_internal_cell(RTLIL::IdString type)
{
	return type[0] == '$' && !type.begins_with("$paramod\\");
}

struct FlowGraph {
	struct Node {
		enum class Type {
			CONNECT,
			CELL,
			PROCESS
		};

		Type type;
		RTLIL::SigSig connect = {};
		const RTLIL::Cell *cell = NULL;
		const RTLIL::Process *process = NULL;
	};

	std::vector<Node*> nodes;
	dict<const RTLIL::Wire*, pool<Node*, hash_ptr_ops>> wire_defs, wire_uses;
	dict<const RTLIL::Wire*, bool> wire_def_elidable, wire_use_elidable;

	~FlowGraph()
	{
		for (auto node : nodes)
			delete node;
	}

	void add_defs(Node *node, const RTLIL::SigSpec &sig, bool elidable)
	{
		for (auto chunk : sig.chunks())
			if (chunk.wire)
				wire_defs[chunk.wire].insert(node);
		// Only defs of an entire wire in the right order can be elided.
		if (sig.is_wire())
			wire_def_elidable[sig.as_wire()] = elidable;
	}

	void add_uses(Node *node, const RTLIL::SigSpec &sig)
	{
		for (auto chunk : sig.chunks())
			if (chunk.wire) {
				wire_uses[chunk.wire].insert(node);
				// Only a single use of an entire wire in the right order can be elided.
				// (But the use can include other chunks.)
				if (!wire_use_elidable.count(chunk.wire))
					wire_use_elidable[chunk.wire] = true;
				else
					wire_use_elidable[chunk.wire] = false;
			}
	}

	bool is_elidable(const RTLIL::Wire *wire) const
	{
		if (wire_def_elidable.count(wire) && wire_use_elidable.count(wire))
			return wire_def_elidable.at(wire) && wire_use_elidable.at(wire);
		return false;
	}

	// Connections
	void add_connect_defs_uses(Node *node, const RTLIL::SigSig &conn)
	{
		add_defs(node, conn.first, /*elidable=*/true);
		add_uses(node, conn.second);
	}

	Node *add_node(const RTLIL::SigSig &conn)
	{
		Node *node = new Node;
		node->type = Node::Type::CONNECT;
		node->connect = conn;
		nodes.push_back(node);
		add_connect_defs_uses(node, conn);
		return node;
	}

	// Cells
	void add_cell_defs_uses(Node *node, const RTLIL::Cell *cell)
	{
		log_assert(cell->known());
		for (auto conn : cell->connections()) {
			if (cell->output(conn.first)) {
				if (is_sync_ff_cell(cell->type) || (cell->type == ID($memrd) && cell->getParam(ID(CLK_ENABLE)).as_bool()))
					/* non-combinatorial outputs do not introduce defs */;
				else if (is_elidable_cell(cell->type))
					add_defs(node, conn.second, /*elidable=*/true);
				else if (is_internal_cell(cell->type))
					add_defs(node, conn.second, /*elidable=*/false);
				else {
					// Unlike outputs of internal cells (which generate code that depends on the ability to set the output
					// wire bits), outputs of user cells are normal wires, and the wires connected to them can be elided.
					add_defs(node, conn.second, /*elidable=*/true);
				}
			}
			if (cell->input(conn.first))
				add_uses(node, conn.second);
		}
	}

	Node *add_node(const RTLIL::Cell *cell)
	{
		Node *node = new Node;
		node->type = Node::Type::CELL;
		node->cell = cell;
		nodes.push_back(node);
		add_cell_defs_uses(node, cell);
		return node;
	}

	// Processes
	void add_case_defs_uses(Node *node, const RTLIL::CaseRule *case_)
	{
		for (auto &action : case_->actions) {
			add_defs(node, action.first, /*elidable=*/false);
			add_uses(node, action.second);
		}
		for (auto sub_switch : case_->switches) {
			add_uses(node, sub_switch->signal);
			for (auto sub_case : sub_switch->cases) {
				for (auto &compare : sub_case->compare)
					add_uses(node, compare);
				add_case_defs_uses(node, sub_case);
			}
		}
	}

	void add_process_defs_uses(Node *node, const RTLIL::Process *process)
	{
		add_case_defs_uses(node, &process->root_case);
		for (auto sync : process->syncs)
			for (auto action : sync->actions) {
				if (sync->type == RTLIL::STp || sync->type == RTLIL::STn || sync->type == RTLIL::STe)
				  /* sync actions do not introduce feedback */;
				else
					add_defs(node, action.first, /*elidable=*/false);
				add_uses(node, action.second);
			}
	}

	Node *add_node(const RTLIL::Process *process)
	{
		Node *node = new Node;
		node->type = Node::Type::PROCESS;
		node->process = process;
		nodes.push_back(node);
		add_process_defs_uses(node, process);
		return node;
	}
};

struct CxxrtlWorker {
	bool split_intf = false;
	std::string intf_filename;
	std::string design_ns = "cxxrtl_design";
	std::ostream *impl_f = nullptr;
	std::ostream *intf_f = nullptr;

	bool elide_internal = false;
	bool elide_public = false;
	bool localize_internal = false;
	bool localize_public = false;
	bool run_splitnets = false;

	std::ostringstream f;
	std::string indent;
	int temporary = 0;

	dict<const RTLIL::Module*, SigMap> sigmaps;
	pool<const RTLIL::Wire*> sync_wires;
	dict<RTLIL::SigBit, RTLIL::SyncType> sync_types;
	pool<const RTLIL::Memory*> writable_memories;
	dict<const RTLIL::Cell*, pool<const RTLIL::Cell*>> transparent_for;
	dict<const RTLIL::Cell*, dict<RTLIL::Wire*, RTLIL::IdString>> cell_wire_defs;
	dict<const RTLIL::Wire*, FlowGraph::Node> elided_wires;
	dict<const RTLIL::Module*, std::vector<FlowGraph::Node>> schedule;
	pool<const RTLIL::Wire*> localized_wires;

	void inc_indent() {
		indent += "\t";
	}
	void dec_indent() {
		indent.resize(indent.size() - 1);
	}

	// RTLIL allows any characters in names other than whitespace. This presents an issue for generating C++ code
	// because C++ identifiers may be only alphanumeric, cannot clash with C++ keywords, and cannot clash with cxxrtl
	// identifiers. This issue can be solved with a name mangling scheme. We choose a name mangling scheme that results
	// in readable identifiers, does not depend on an up-to-date list of C++ keywords, and is easy to apply. Its rules:
	//  1. All generated identifiers start with `_`.
	//  1a. Generated identifiers for public names (beginning with `\`) start with `p_`.
	//  1b. Generated identifiers for internal names (beginning with `$`) start with `i_`.
	//  2. An underscore is escaped with another underscore, i.e. `__`.
	//  3. Any other non-alnum character is escaped with underscores around its lowercase hex code, e.g. `@` as `_40_`.
	std::string mangle_name(const RTLIL::IdString &name)
	{
		std::string mangled;
		bool first = true;
		for (char c : name.str()) {
			if (first) {
				first = false;
				if (c == '\\')
					mangled += "p_";
				else if (c == '$')
					mangled += "i_";
				else
					log_assert(false);
			} else {
				if (isalnum(c)) {
					mangled += c;
				} else if (c == '_') {
					mangled += "__";
				} else {
					char l = c & 0xf, h = (c >> 4) & 0xf;
					mangled += '_';
					mangled += (h < 10 ? '0' + h : 'a' + h - 10);
					mangled += (l < 10 ? '0' + l : 'a' + l - 10);
					mangled += '_';
				}
			}
		}
		return mangled;
	}

	std::string mangle_module_name(const RTLIL::IdString &name)
	{
		// Class namespace.
		return mangle_name(name);
	}

	std::string mangle_memory_name(const RTLIL::IdString &name)
	{
		// Class member namespace.
		return "memory_" + mangle_name(name);
	}

	std::string mangle_cell_name(const RTLIL::IdString &name)
	{
		// Class member namespace.
		return "cell_" + mangle_name(name);
	}

	std::string mangle_wire_name(const RTLIL::IdString &name)
	{
		// Class member namespace.
		return mangle_name(name);
	}

	std::string mangle(const RTLIL::Module *module)
	{
		return mangle_module_name(module->name);
	}

	std::string mangle(const RTLIL::Memory *memory)
	{
		return mangle_memory_name(memory->name);
	}

	std::string mangle(const RTLIL::Cell *cell)
	{
		return mangle_cell_name(cell->name);
	}

	std::string mangle(const RTLIL::Wire *wire)
	{
		return mangle_wire_name(wire->name);
	}

	std::string mangle(RTLIL::SigBit sigbit)
	{
		log_assert(sigbit.wire != NULL);
		if (sigbit.wire->width == 1)
			return mangle(sigbit.wire);
		return mangle(sigbit.wire) + "_" + std::to_string(sigbit.offset);
	}

	std::string fresh_temporary()
	{
		return stringf("tmp_%d", temporary++);
	}

	void dump_attrs(const RTLIL::AttrObject *object)
	{
		for (auto attr : object->attributes) {
			f << indent << "// " << attr.first.str() << ": ";
			if (attr.second.flags & RTLIL::CONST_FLAG_STRING) {
				f << attr.second.decode_string();
			} else {
				f << attr.second.as_int(/*is_signed=*/attr.second.flags & RTLIL::CONST_FLAG_SIGNED);
			}
			f << "\n";
		}
	}

	void dump_const_init(const RTLIL::Const &data, int width, int offset = 0, bool fixed_width = false)
	{
		f << "{";
		while (width > 0) {
			const int CHUNK_SIZE = 32;
			uint32_t chunk = data.extract(offset, width > CHUNK_SIZE ? CHUNK_SIZE : width).as_int();
			if (fixed_width)
				f << stringf("0x%08xu", chunk);
			else
				f << stringf("%#xu", chunk);
			if (width > CHUNK_SIZE)
				f << ',';
			offset += CHUNK_SIZE;
			width  -= CHUNK_SIZE;
		}
		f << "}";
	}

	void dump_const_init(const RTLIL::Const &data)
	{
		dump_const_init(data, data.size());
	}

	void dump_const(const RTLIL::Const &data, int width, int offset = 0, bool fixed_width = false)
	{
		f << "value<" << width << ">";
		dump_const_init(data, width, offset, fixed_width);
	}

	void dump_const(const RTLIL::Const &data)
	{
		dump_const(data, data.size());
	}

	bool dump_sigchunk(const RTLIL::SigChunk &chunk, bool is_lhs)
	{
		if (chunk.wire == NULL) {
			dump_const(chunk.data, chunk.width, chunk.offset);
			return false;
		} else {
			if (!is_lhs && elided_wires.count(chunk.wire)) {
				const FlowGraph::Node &node = elided_wires[chunk.wire];
				switch (node.type) {
					case FlowGraph::Node::Type::CONNECT:
						dump_connect_elided(node.connect);
						break;
					case FlowGraph::Node::Type::CELL:
						if (is_elidable_cell(node.cell->type)) {
							dump_cell_elided(node.cell);
						} else {
							f << mangle(node.cell) << "." << mangle_wire_name(cell_wire_defs[node.cell][chunk.wire]) << ".curr";
						}
						break;
					default:
						log_assert(false);
				}
			} else if (localized_wires[chunk.wire]) {
				f << mangle(chunk.wire);
			} else {
				f << mangle(chunk.wire) << (is_lhs ? ".next" : ".curr");
			}
			if (chunk.width == chunk.wire->width && chunk.offset == 0)
				return false;
			else if (chunk.width == 1)
				f << ".slice<" << chunk.offset << ">()";
			else
				f << ".slice<" << chunk.offset+chunk.width-1 << "," << chunk.offset << ">()";
			return true;
		}
	}

	bool dump_sigspec(const RTLIL::SigSpec &sig, bool is_lhs)
	{
		if (sig.empty()) {
			f << "value<0>()";
			return false;
		} else if (sig.is_chunk()) {
			return dump_sigchunk(sig.as_chunk(), is_lhs);
		} else {
			dump_sigchunk(*sig.chunks().rbegin(), is_lhs);
			for (auto it = sig.chunks().rbegin() + 1; it != sig.chunks().rend(); ++it) {
				f << ".concat(";
				dump_sigchunk(*it, is_lhs);
				f << ")";
			}
			return true;
		}
	}

	void dump_sigspec_lhs(const RTLIL::SigSpec &sig)
	{
		dump_sigspec(sig, /*is_lhs=*/true);
	}

	void dump_sigspec_rhs(const RTLIL::SigSpec &sig)
	{
		// In the contexts where we want template argument deduction to occur for `template<size_t Bits> ... value<Bits>`,
		// it is necessary to have the argument to already be a `value<N>`, since template argument deduction and implicit
		// type conversion are mutually exclusive. In these contexts, we use dump_sigspec_rhs() to emit an explicit
		// type conversion, but only if the expression needs it.
		bool is_complex = dump_sigspec(sig, /*is_lhs=*/false);
		if (is_complex)
			f << ".val()";
	}

	void collect_sigspec_rhs(const RTLIL::SigSpec &sig, std::vector<RTLIL::IdString> &cells)
	{
		for (auto chunk : sig.chunks()) {
			if (!chunk.wire || !elided_wires.count(chunk.wire))
				continue;

			const FlowGraph::Node &node = elided_wires[chunk.wire];
			switch (node.type) {
				case FlowGraph::Node::Type::CONNECT:
					collect_connect(node.connect, cells);
					break;
				case FlowGraph::Node::Type::CELL:
					collect_cell(node.cell, cells);
					break;
				default:
					log_assert(false);
			}
		}
	}

	void dump_connect_elided(const RTLIL::SigSig &conn)
	{
		dump_sigspec_rhs(conn.second);
	}

	bool is_connect_elided(const RTLIL::SigSig &conn)
	{
		return conn.first.is_wire() && elided_wires.count(conn.first.as_wire());
	}

	void collect_connect(const RTLIL::SigSig &conn, std::vector<RTLIL::IdString> &cells)
	{
		if (!is_connect_elided(conn))
			return;

		collect_sigspec_rhs(conn.second, cells);
	}

	void dump_connect(const RTLIL::SigSig &conn)
	{
		if (is_connect_elided(conn))
			return;

		f << indent << "// connection\n";
		f << indent;
		dump_sigspec_lhs(conn.first);
		f << " = ";
		dump_connect_elided(conn);
		f << ";\n";
	}

	void dump_cell_elided(const RTLIL::Cell *cell)
	{
		// Unary cells
		if (is_unary_cell(cell->type)) {
			f << cell->type.substr(1) << '_' <<
			     (cell->getParam(ID(A_SIGNED)).as_bool() ? 's' : 'u') <<
			     "<" << cell->getParam(ID(Y_WIDTH)).as_int() << ">(";
			dump_sigspec_rhs(cell->getPort(ID(A)));
			f << ")";
		// Binary cells
		} else if (is_binary_cell(cell->type)) {
			f << cell->type.substr(1) << '_' <<
			     (cell->getParam(ID(A_SIGNED)).as_bool() ? 's' : 'u') <<
			     (cell->getParam(ID(B_SIGNED)).as_bool() ? 's' : 'u') <<
			     "<" << cell->getParam(ID(Y_WIDTH)).as_int() << ">(";
			dump_sigspec_rhs(cell->getPort(ID(A)));
			f << ", ";
			dump_sigspec_rhs(cell->getPort(ID(B)));
			f << ")";
		// Muxes
		} else if (cell->type == ID($mux)) {
			f << "(";
			dump_sigspec_rhs(cell->getPort(ID(S)));
			f << " ? ";
			dump_sigspec_rhs(cell->getPort(ID(B)));
			f << " : ";
			dump_sigspec_rhs(cell->getPort(ID(A)));
			f << ")";
		// Concats
		} else if (cell->type == ID($concat)) {
			dump_sigspec_rhs(cell->getPort(ID(B)));
			f << ".concat(";
			dump_sigspec_rhs(cell->getPort(ID(A)));
			f << ").val()";
		// Slices
		} else if (cell->type == ID($slice)) {
			dump_sigspec_rhs(cell->getPort(ID(A)));
			f << ".slice<";
			f << cell->getParam(ID(OFFSET)).as_int() + cell->getParam(ID(Y_WIDTH)).as_int() - 1;
			f << ",";
			f << cell->getParam(ID(OFFSET)).as_int();
			f << ">().val()";
		} else {
			log_assert(false);
		}
	}

	bool is_cell_elided(const RTLIL::Cell *cell)
	{
		return is_elidable_cell(cell->type) && cell->hasPort(ID(Y)) && cell->getPort(ID(Y)).is_wire() &&
			elided_wires.count(cell->getPort(ID(Y)).as_wire());
	}

	void collect_cell(const RTLIL::Cell *cell, std::vector<RTLIL::IdString> &cells)
	{
		if (!is_cell_elided(cell))
			return;

		cells.push_back(cell->name);
		for (auto port : cell->connections())
			if (port.first != ID(Y))
				collect_sigspec_rhs(port.second, cells);
	}

	void dump_cell(const RTLIL::Cell *cell)
	{
		if (is_cell_elided(cell))
			return;
		if (cell->type == ID($meminit))
			return; // Handled elsewhere.

		std::vector<RTLIL::IdString> elided_cells;
		if (is_elidable_cell(cell->type)) {
			for (auto port : cell->connections())
				if (port.first != ID(Y))
					collect_sigspec_rhs(port.second, elided_cells);
		}
		if (elided_cells.empty()) {
			dump_attrs(cell);
			f << indent << "// cell " << cell->name.str() << "\n";
		} else {
			f << indent << "// cells";
			for (auto elided_cell : elided_cells)
				f << " " << elided_cell.str();
			f << "\n";
		}

		// Elidable cells
		if (is_elidable_cell(cell->type)) {
			f << indent;
			dump_sigspec_lhs(cell->getPort(ID(Y)));
			f << " = ";
			dump_cell_elided(cell);
			f << ";\n";
		// Parallel (one-hot) muxes
		} else if (cell->type == ID($pmux)) {
			int width = cell->getParam(ID(WIDTH)).as_int();
			int s_width = cell->getParam(ID(S_WIDTH)).as_int();
			bool first = true;
			for (int part = 0; part < s_width; part++) {
				f << (first ? indent : " else ");
				first = false;
				f << "if (";
				dump_sigspec_rhs(cell->getPort(ID(S)).extract(part));
				f << ") {\n";
				inc_indent();
					f << indent;
					dump_sigspec_lhs(cell->getPort(ID(Y)));
					f << " = ";
					dump_sigspec_rhs(cell->getPort(ID(B)).extract(part * width, width));
					f << ";\n";
				dec_indent();
				f << indent << "}";
			}
			f << " else {\n";
			inc_indent();
				f << indent;
				dump_sigspec_lhs(cell->getPort(ID(Y)));
				f << " = ";
				dump_sigspec_rhs(cell->getPort(ID(A)));
				f << ";\n";
			dec_indent();
			f << indent << "}\n";
		// Flip-flops
		} else if (is_ff_cell(cell->type)) {
			if (cell->hasPort(ID(CLK)) && cell->getPort(ID(CLK)).is_wire()) {
				// Edge-sensitive logic
				RTLIL::SigBit clk_bit = cell->getPort(ID(CLK))[0];
				clk_bit = sigmaps[clk_bit.wire->module](clk_bit);
				f << indent << "if (" << (cell->getParam(ID(CLK_POLARITY)).as_bool() ? "posedge_" : "negedge_")
				            << mangle(clk_bit) << ") {\n";
				inc_indent();
					if (cell->type == ID($dffe)) {
						f << indent << "if (";
						dump_sigspec_rhs(cell->getPort(ID(EN)));
						f << " == value<1> {" << cell->getParam(ID(EN_POLARITY)).as_bool() << "u}) {\n";
						inc_indent();
					}
					f << indent;
					dump_sigspec_lhs(cell->getPort(ID(Q)));
					f << " = ";
					dump_sigspec_rhs(cell->getPort(ID(D)));
					f << ";\n";
					if (cell->type == ID($dffe)) {
						dec_indent();
						f << indent << "}\n";
					}
				dec_indent();
				f << indent << "}\n";
			} else if (cell->hasPort(ID(EN))) {
				// Level-sensitive logic
				f << indent << "if (";
				dump_sigspec_rhs(cell->getPort(ID(EN)));
				f << " == value<1> {" << cell->getParam(ID(EN_POLARITY)).as_bool() << "u}) {\n";
				inc_indent();
					f << indent;
					dump_sigspec_lhs(cell->getPort(ID(Q)));
					f << " = ";
					dump_sigspec_rhs(cell->getPort(ID(D)));
					f << ";\n";
				dec_indent();
				f << indent << "}\n";
			}
			if (cell->hasPort(ID(ARST))) {
				// Asynchronous reset (entire coarse cell at once)
				f << indent << "if (";
				dump_sigspec_rhs(cell->getPort(ID(ARST)));
				f << " == value<1> {" << cell->getParam(ID(ARST_POLARITY)).as_bool() << "u}) {\n";
				inc_indent();
					f << indent;
					dump_sigspec_lhs(cell->getPort(ID(Q)));
					f << " = ";
					dump_const(cell->getParam(ID(ARST_VALUE)));
					f << ";\n";
				dec_indent();
				f << indent << "}\n";
			}
			if (cell->hasPort(ID(SET))) {
				// Asynchronous set (for individual bits)
				f << indent;
				dump_sigspec_lhs(cell->getPort(ID(Q)));
				f << " = ";
				dump_sigspec_lhs(cell->getPort(ID(Q)));
				f << ".update(";
				dump_const(RTLIL::Const(RTLIL::S1, cell->getParam(ID(WIDTH)).as_int()));
				f << ", ";
				dump_sigspec_rhs(cell->getPort(ID(SET)));
				f << (cell->getParam(ID(SET_POLARITY)).as_bool() ? "" : ".bit_not()") << ");\n";
			}
			if (cell->hasPort(ID(CLR))) {
				// Asynchronous clear (for individual bits; priority over set)
				f << indent;
				dump_sigspec_lhs(cell->getPort(ID(Q)));
				f << " = ";
				dump_sigspec_lhs(cell->getPort(ID(Q)));
				f << ".update(";
				dump_const(RTLIL::Const(RTLIL::S0, cell->getParam(ID(WIDTH)).as_int()));
				f << ", ";
				dump_sigspec_rhs(cell->getPort(ID(CLR)));
				f << (cell->getParam(ID(CLR_POLARITY)).as_bool() ? "" : ".bit_not()") << ");\n";
			}
		// Memory ports
		} else if (cell->type.in(ID($memrd), ID($memwr))) {
			if (cell->getParam(ID(CLK_ENABLE)).as_bool()) {
				RTLIL::SigBit clk_bit = cell->getPort(ID(CLK))[0];
				clk_bit = sigmaps[clk_bit.wire->module](clk_bit);
				f << indent << "if (" << (cell->getParam(ID(CLK_POLARITY)).as_bool() ? "posedge_" : "negedge_")
				            << mangle(clk_bit) << ") {\n";
				inc_indent();
			}
			RTLIL::Memory *memory = cell->module->memories[cell->getParam(ID(MEMID)).decode_string()];
			std::string valid_index_temp = fresh_temporary();
			f << indent << "auto " << valid_index_temp << " = memory_index(";
			dump_sigspec_rhs(cell->getPort(ID(ADDR)));
			f << ", " << memory->start_offset << ", " << memory->size << ");\n";
			if (cell->type == ID($memrd)) {
				if (!cell->getPort(ID(EN)).is_fully_ones()) {
					f << indent << "if (";
					dump_sigspec_rhs(cell->getPort(ID(EN)));
					f << ") {\n";
					inc_indent();
				}
				// The generated code has two bounds checks; one in an assertion, and another that guards the read.
				// This is done so that the code does not invoke undefined behavior under any conditions, but nevertheless
				// loudly crashes if an illegal condition is encountered. The assert may be turned off with -NDEBUG not
				// just for release builds, but also to make sure the simulator (which is presumably embedded in some
				// larger program) will never crash the code that calls into it.
				//
				// If assertions are disabled, out of bounds reads are defined to return zero.
				f << indent << "assert(" << valid_index_temp << ".valid && \"out of bounds read\");\n";
				f << indent << "if(" << valid_index_temp << ".valid) {\n";
				inc_indent();
					if (writable_memories[memory]) {
						std::string addr_temp = fresh_temporary();
						f << indent << "const value<" << cell->getPort(ID(ADDR)).size() << "> &" << addr_temp << " = ";
						dump_sigspec_rhs(cell->getPort(ID(ADDR)));
						f << ";\n";
						std::string lhs_temp = fresh_temporary();
						f << indent << "value<" << memory->width << "> " << lhs_temp << " = "
						            << mangle(memory) << "[" << valid_index_temp << ".index];\n";
						std::vector<const RTLIL::Cell*> memwr_cells(transparent_for[cell].begin(), transparent_for[cell].end());
						std::sort(memwr_cells.begin(), memwr_cells.end(),
							[](const RTLIL::Cell *a, const RTLIL::Cell *b) {
								return a->getParam(ID(PRIORITY)).as_int() < b->getParam(ID(PRIORITY)).as_int();
							});
						for (auto memwr_cell : memwr_cells) {
							f << indent << "if (" << addr_temp << " == ";
							dump_sigspec_rhs(memwr_cell->getPort(ID(ADDR)));
							f << ") {\n";
							inc_indent();
								f << indent << lhs_temp << " = " << lhs_temp;
								f << ".update(";
								dump_sigspec_rhs(memwr_cell->getPort(ID(DATA)));
								f << ", ";
								dump_sigspec_rhs(memwr_cell->getPort(ID(EN)));
								f << ");\n";
							dec_indent();
							f << indent << "}\n";
						}
						f << indent;
						dump_sigspec_lhs(cell->getPort(ID(DATA)));
						f << " = " << lhs_temp << ";\n";
					} else {
						f << indent;
						dump_sigspec_lhs(cell->getPort(ID(DATA)));
						f << " = " << mangle(memory) << "[" << valid_index_temp << ".index];\n";
					}
				dec_indent();
				f << indent << "} else {\n";
				inc_indent();
					f << indent;
					dump_sigspec_lhs(cell->getPort(ID(DATA)));
					f << " = value<" << memory->width << "> {};\n";
				dec_indent();
				f << indent << "}\n";
				if (!cell->getPort(ID(EN)).is_fully_ones()) {
					dec_indent();
					f << indent << "}\n";
				}
			} else /*if (cell->type == ID($memwr))*/ {
				log_assert(writable_memories[memory]);
				// See above for rationale of having both the assert and the condition.
				//
				// If assertions are disabled, out of bounds writes are defined to do nothing.
				f << indent << "assert(" << valid_index_temp << ".valid && \"out of bounds write\");\n";
				f << indent << "if (" << valid_index_temp << ".valid) {\n";
				inc_indent();
					f << indent << mangle(memory) << ".update(" << valid_index_temp << ".index, ";
					dump_sigspec_rhs(cell->getPort(ID(DATA)));
					f << ", ";
					dump_sigspec_rhs(cell->getPort(ID(EN)));
					f << ", " << cell->getParam(ID(PRIORITY)).as_int() << ");\n";
				dec_indent();
				f << indent << "}\n";
			}
			if (cell->getParam(ID(CLK_ENABLE)).as_bool()) {
				dec_indent();
				f << indent << "}\n";
			}
		// Internal cells
		} else if (is_internal_cell(cell->type)) {
			log_cmd_error("Unsupported internal cell `%s'.\n", cell->type.c_str());
		// User cells
		} else {
			log_assert(cell->known());
			for (auto conn : cell->connections())
				if (cell->input(conn.first)) {
					f << indent << mangle(cell) << "." << mangle_wire_name(conn.first) << ".next = ";
					dump_sigspec_rhs(conn.second);
					f << ";\n";
				}
			f << indent << mangle(cell) << ".eval();\n";
			for (auto conn : cell->connections()) {
				if (conn.second.is_wire()) {
					RTLIL::Wire *wire = conn.second.as_wire();
					if (elided_wires.count(wire) && cell_wire_defs[cell].count(wire))
						continue;
				}
				if (cell->output(conn.first)) {
					f << indent;
					dump_sigspec_lhs(conn.second);
					f << " = " << mangle(cell) << "." << mangle_wire_name(conn.first) << ".curr;\n";
				}
			}
		}
	}

	void dump_assign(const RTLIL::SigSig &sigsig)
	{
		f << indent;
		dump_sigspec_lhs(sigsig.first);
		f << " = ";
		dump_sigspec_rhs(sigsig.second);
		f << ";\n";
	}

	void dump_case_rule(const RTLIL::CaseRule *rule)
	{
		for (auto action : rule->actions)
			dump_assign(action);
		for (auto switch_ : rule->switches)
			dump_switch_rule(switch_);
	}

	void dump_switch_rule(const RTLIL::SwitchRule *rule)
	{
		// The switch attributes are printed before the switch condition is captured.
		dump_attrs(rule);
		std::string signal_temp = fresh_temporary();
		f << indent << "const value<" << rule->signal.size() << "> &" << signal_temp << " = ";
		dump_sigspec(rule->signal, /*is_lhs=*/false);
		f << ";\n";

		bool first = true;
		for (auto case_ : rule->cases) {
			// The case attributes (for nested cases) are printed before the if/else if/else statement.
			dump_attrs(rule);
			f << indent;
			if (!first)
				f << "} else ";
			first = false;
			if (!case_->compare.empty()) {
				f << "if (";
				bool first = true;
				for (auto &compare : case_->compare) {
					if (!first)
						f << " || ";
					first = false;
					if (compare.is_fully_def()) {
						f << signal_temp << " == ";
						dump_sigspec(compare, /*is_lhs=*/false);
					} else if (compare.is_fully_const()) {
						RTLIL::Const compare_mask, compare_value;
						for (auto bit : compare.as_const()) {
							switch (bit) {
								case RTLIL::S0:
								case RTLIL::S1:
									compare_mask.bits.push_back(RTLIL::S1);
									compare_value.bits.push_back(bit);
									break;

								case RTLIL::Sx:
								case RTLIL::Sz:
								case RTLIL::Sa:
									compare_mask.bits.push_back(RTLIL::S0);
									compare_value.bits.push_back(RTLIL::S0);
									break;

								default:
									log_assert(false);
							}
						}
						f << "and_uu<" << compare.size() << ">(" << signal_temp << ", ";
						dump_const(compare_mask);
						f << ") == ";
						dump_const(compare_value);
					} else {
						log_assert(false);
					}
				}
				f << ") ";
			}
			f << "{\n";
			inc_indent();
				dump_case_rule(case_);
			dec_indent();
		}
		f << indent << "}\n";
	}

	void dump_process(const RTLIL::Process *proc)
	{
		dump_attrs(proc);
		f << indent << "// process " << proc->name.str() << "\n";
		// The case attributes (for root case) are always empty.
		log_assert(proc->root_case.attributes.empty());
		dump_case_rule(&proc->root_case);
		for (auto sync : proc->syncs) {
			RTLIL::SigBit sync_bit = sync->signal[0];
			sync_bit = sigmaps[sync_bit.wire->module](sync_bit);

			pool<std::string> events;
			switch (sync->type) {
				case RTLIL::STp:
					events.insert("posedge_" + mangle(sync_bit));
					break;
				case RTLIL::STn:
					events.insert("negedge_" + mangle(sync_bit));
				case RTLIL::STe:
					events.insert("posedge_" + mangle(sync_bit));
					events.insert("negedge_" + mangle(sync_bit));
					break;

				case RTLIL::ST0:
				case RTLIL::ST1:
				case RTLIL::STa:
				case RTLIL::STg:
				case RTLIL::STi:
					log_assert(false);
			}
			if (!events.empty()) {
				f << indent << "if (";
				bool first = true;
				for (auto &event : events) {
					if (!first)
						f << " || ";
					first = false;
					f << event;
				}
				f << ") {\n";
				inc_indent();
					for (auto action : sync->actions)
						dump_assign(action);
				dec_indent();
				f << indent << "}\n";
			}
		}
	}

	void dump_wire(const RTLIL::Wire *wire, bool is_local)
	{
		if (elided_wires.count(wire))
			return;

		if (is_local) {
			if (!localized_wires.count(wire))
				return;

			dump_attrs(wire);
			f << indent << "value<" << wire->width << "> " << mangle(wire) << ";\n";
		} else {
			if (localized_wires.count(wire))
				return;

			dump_attrs(wire);
			f << indent << "wire<" << wire->width << "> " << mangle(wire);
			if (wire->attributes.count(ID(init))) {
				f << " ";
				dump_const_init(wire->attributes.at(ID(init)));
			}
			f << ";\n";
			if (sync_wires[wire]) {
				for (auto sync_type : sync_types) {
					if (sync_type.first.wire == wire) {
						if (sync_type.second != RTLIL::STn)
							f << indent << "bool posedge_" << mangle(sync_type.first) << " = false;\n";
						if (sync_type.second != RTLIL::STp)
							f << indent << "bool negedge_" << mangle(sync_type.first) << " = false;\n";
					}
				}
			}
		}
	}

	void dump_memory(RTLIL::Module *module, const RTLIL::Memory *memory)
	{
		vector<const RTLIL::Cell*> init_cells;
		for (auto cell : module->cells())
			if (cell->type == ID($meminit) && cell->getParam(ID(MEMID)).decode_string() == memory->name.str())
				init_cells.push_back(cell);

		std::sort(init_cells.begin(), init_cells.end(), [](const RTLIL::Cell *a, const RTLIL::Cell *b) {
			int a_addr = a->getPort(ID(ADDR)).as_int(), b_addr = b->getPort(ID(ADDR)).as_int();
			int a_prio = a->getParam(ID(PRIORITY)).as_int(), b_prio = b->getParam(ID(PRIORITY)).as_int();
			return a_prio > b_prio || (a_prio == b_prio && a_addr < b_addr);
		});

		dump_attrs(memory);
		f << indent << (writable_memories[memory] ? "" : "const ")
		            << "memory<" << memory->width << "> " << mangle(memory)
		            << " { " << memory->size << "u";
		if (init_cells.empty()) {
			f << " };\n";
		} else {
			f << ",\n";
			inc_indent();
				for (auto cell : init_cells) {
					dump_attrs(cell);
					RTLIL::Const data = cell->getPort(ID(DATA)).as_const();
					size_t width = cell->getParam(ID(WIDTH)).as_int();
					size_t words = cell->getParam(ID(WORDS)).as_int();
					f << indent << "memory<" << memory->width << ">::init<" << words << "> { "
					            << stringf("%#x", cell->getPort(ID(ADDR)).as_int()) << ", {";
					inc_indent();
						for (size_t n = 0; n < words; n++) {
							if (n % 4 == 0)
								f << "\n" << indent;
							else
								f << " ";
							dump_const(data, width, n * width, /*fixed_width=*/true);
							f << ",";
						}
					dec_indent();
					f << "\n" << indent << "}},\n";
				}
			dec_indent();
			f << indent << "};\n";
		}
	}

	void dump_module_intf(RTLIL::Module *module)
	{
		dump_attrs(module);
		f << "struct " << mangle(module) << " : public module {\n";
		inc_indent();
			for (auto wire : module->wires())
				dump_wire(wire, /*is_local=*/false);
			f << "\n";
			bool has_memories = false;
			for (auto memory : module->memories) {
				dump_memory(module, memory.second);
				has_memories = true;
			}
			if (has_memories)
				f << "\n";
			bool has_cells = false;
			for (auto cell : module->cells()) {
				if (is_internal_cell(cell->type))
					continue;
				f << indent << mangle_module_name(cell->type) << " " << mangle(cell) << ";\n";
				has_cells = true;
			}
			if (has_cells)
				f << "\n";
			f << indent << "void eval() override;\n";
			f << indent << "bool commit() override;\n";
		dec_indent();
		f << "}; // struct " << mangle(module) << "\n";
		f << "\n";
	}

	void dump_module_impl(RTLIL::Module *module)
	{
		f << "void " << mangle(module) << "::eval() {\n";
		inc_indent();
			for (auto wire : module->wires())
				dump_wire(wire, /*is_local=*/true);
			for (auto node : schedule[module]) {
				switch (node.type) {
					case FlowGraph::Node::Type::CONNECT:
						dump_connect(node.connect);
						break;
					case FlowGraph::Node::Type::CELL:
						dump_cell(node.cell);
						break;
					case FlowGraph::Node::Type::PROCESS:
						dump_process(node.process);
						break;
				}
			}
			for (auto sync_type : sync_types) {
				if (sync_type.first.wire->module == module) {
					if (sync_type.second != RTLIL::STn)
						f << indent << "posedge_" << mangle(sync_type.first) << " = false;\n";
					if (sync_type.second != RTLIL::STp)
						f << indent << "negedge_" << mangle(sync_type.first) << " = false;\n";
				}
			}
		dec_indent();
		f << "}\n";
		f << "\n";

		f << "bool " << mangle(module) << "::commit() {\n";
		inc_indent();
			f << indent << "bool changed = false;\n";
			for (auto wire : module->wires()) {
				if (elided_wires.count(wire) || localized_wires.count(wire))
					continue;
				if (sync_wires[wire]) {
					std::string wire_prev = mangle(wire) + "_prev";
					std::string wire_curr = mangle(wire) + ".curr";
					std::string wire_edge = mangle(wire) + "_edge";
					f << indent << "value<" << wire->width << "> " << wire_prev << " = " << wire_curr << ";\n";
					f << indent << "if (" << mangle(wire) << ".commit()) {\n";
					inc_indent();
						f << indent << "value<" << wire->width << "> " << wire_edge << " = "
						            << wire_prev << ".bit_xor(" << wire_curr << ");\n";
						for (auto sync_type : sync_types) {
							if (sync_type.first.wire != wire)
								continue;
							if (sync_type.second != RTLIL::STn) {
								f << indent << "if (" << wire_edge << ".slice<" << sync_type.first.offset << ">().val() && "
								            << wire_curr << ".slice<" << sync_type.first.offset << ">().val())\n";
								inc_indent();
									f << indent << "posedge_" << mangle(sync_type.first) << " = true;\n";
								dec_indent();
							}
							if (sync_type.second != RTLIL::STp) {
								f << indent << "if (" << wire_edge << ".slice<" << sync_type.first.offset << ">().val() && "
								            << "!" << wire_curr << ".slice<" << sync_type.first.offset << ">().val())\n";
								inc_indent();
									f << indent << "negedge_" << mangle(sync_type.first) << " = true;\n";
								dec_indent();
							}
							f << indent << "changed = true;\n";
						}
					dec_indent();
					f << indent << "}\n";
				} else {
					f << indent << "changed |= " << mangle(wire) << ".commit();\n";
				}
			}
			for (auto memory : module->memories) {
				if (!writable_memories[memory.second])
					continue;
				f << indent << "changed |= " << mangle(memory.second) << ".commit();\n";
			}
			for (auto cell : module->cells()) {
				if (is_internal_cell(cell->type))
					continue;
				f << indent << "changed |= " << mangle(cell) << ".commit();\n";
			}
			f << indent << "return changed;\n";
		dec_indent();
		f << "}\n";
		f << "\n";
	}

	void dump_design(RTLIL::Design *design)
	{
		TopoSort<RTLIL::Module*> topo_design;
		for (auto module : design->modules()) {
			if (module->get_blackbox_attribute() || !design->selected_module(module))
				continue;
			topo_design.node(module);

			for (auto cell : module->cells()) {
				if (is_internal_cell(cell->type))
					continue;
				log_assert(design->has(cell->type));
				topo_design.edge(design->module(cell->type), module);
			}
		}
		log_assert(topo_design.sort());

		if (split_intf) {
			// The only thing more depraved than include guards, is mangling filenames to turn them into include guards.
			std::string include_guard = design_ns + "_header";
			std::transform(include_guard.begin(), include_guard.end(), include_guard.begin(), ::toupper);

			f << "#ifndef " << include_guard << "\n";
			f << "#define " << include_guard << "\n";
			f << "\n";
			f << "#include <backends/cxxrtl/cxxrtl.h>\n";
			f << "\n";
			f << "using namespace cxxrtl;\n";
			f << "\n";
			f << "namespace " << design_ns << " {\n";
			f << "\n";
			for (auto module : topo_design.sorted) {
				if (!design->selected_module(module))
					continue;
				dump_module_intf(module);
			}
			f << "} // namespace " << design_ns << "\n";
			f << "\n";
			f << "#endif\n";
			*intf_f << f.str(); f.str("");
		}

		if (split_intf)
			f << "#include \"" << intf_filename << "\"\n";
		else
			f << "#include <backends/cxxrtl/cxxrtl.h>\n";
		f << "\n";
		f << "using namespace cxxrtl_yosys;\n";
		f << "\n";
		f << "namespace " << design_ns << " {\n";
		f << "\n";
		for (auto module : topo_design.sorted) {
			if (!design->selected_module(module))
				continue;
			if (!split_intf)
				dump_module_intf(module);
			dump_module_impl(module);
		}
		f << "} // namespace " << design_ns << "\n";
		*impl_f << f.str(); f.str("");
	}

	// Edge-type sync rules require us to emit edge detectors, which require coordination between
	// eval and commit phases. To do this we need to collect them upfront.
	//
	// Note that the simulator commit phase operates at wire granularity but edge-type sync rules
	// operate at wire bit granularity; it is possible to have code similar to:
	//     wire [3:0] clocks;
	//     always @(posedge clocks[0]) ...
	// To handle this we track edge sensitivity both for wires and wire bits.
	void register_edge_signal(SigMap &sigmap, RTLIL::SigSpec signal, RTLIL::SyncType type)
	{
		signal = sigmap(signal);
		log_assert(signal.is_wire() && signal.is_bit());
		log_assert(type == RTLIL::STp || type == RTLIL::STn || type == RTLIL::STe);

		RTLIL::SigBit sigbit = signal[0];
		if (!sync_types.count(sigbit))
			sync_types[sigbit] = type;
		else if (sync_types[sigbit] != type)
			sync_types[sigbit] = RTLIL::STe;
		sync_wires.insert(signal.as_wire());
	}

	void analyze_design(RTLIL::Design *design)
	{
		bool has_feedback_arcs = false;
		for (auto module : design->modules()) {
			if (!design->selected_module(module))
				continue;

			FlowGraph flow;
			SigMap &sigmap = sigmaps[module];
			sigmap.set(module);

			for (auto conn : module->connections())
				flow.add_node(conn);

			dict<const RTLIL::Cell*, FlowGraph::Node*> memrw_cell_nodes;
			dict<std::pair<RTLIL::SigBit, const RTLIL::Memory*>,
			     pool<const RTLIL::Cell*>> memwr_per_domain;
			for (auto cell : module->cells()) {
				FlowGraph::Node *node = flow.add_node(cell);

				// Various DFF cells are treated like posedge/negedge processes, see above for details.
				if (cell->type.in(ID($dff), ID($dffe), ID($adff), ID($dffsr))) {
					if (cell->getPort(ID(CLK)).is_wire())
						register_edge_signal(sigmap, cell->getPort(ID(CLK)),
							cell->parameters[ID(CLK_POLARITY)].as_bool() ? RTLIL::STp : RTLIL::STn);
					// The $adff and $dffsr cells are level-sensitive, not edge-sensitive (in spite of the fact that they
					// are inferred from an edge-sensitive Verilog process) and do not correspond to an edge-type sync rule.
				}
				// Similar for memory port cells.
				if (cell->type.in(ID($memrd), ID($memwr))) {
					if (cell->getParam(ID(CLK_ENABLE)).as_bool()) {
						if (cell->getPort(ID(CLK)).is_wire())
							register_edge_signal(sigmap, cell->getPort(ID(CLK)),
								cell->parameters[ID(CLK_POLARITY)].as_bool() ? RTLIL::STp : RTLIL::STn);
					}
					memrw_cell_nodes[cell] = node;
				}
				// Optimize access to read-only memories.
				if (cell->type == ID($memwr))
					writable_memories.insert(module->memories[cell->getParam(ID(MEMID)).decode_string()]);
				// Collect groups of memory write ports in the same domain.
				if (cell->type == ID($memwr) && cell->getParam(ID(CLK_ENABLE)).as_bool() && cell->getPort(ID(CLK)).is_wire()) {
					RTLIL::SigBit clk_bit = sigmap(cell->getPort(ID(CLK)))[0];
					const RTLIL::Memory *memory = module->memories[cell->getParam(ID(MEMID)).decode_string()];
					memwr_per_domain[{clk_bit, memory}].insert(cell);
				}
				// Handling of packed memories is delegated to the `memory_unpack` pass, so we can rely on the presence
				// of RTLIL memory objects and $memrd/$memwr/$meminit cells.
				if (cell->type.in(ID($mem)))
					log_assert(false);
			}
			for (auto cell : module->cells()) {
				// Collect groups of memory write ports read by every transparent read port.
				if (cell->type == ID($memrd) && cell->getParam(ID(CLK_ENABLE)).as_bool() && cell->getPort(ID(CLK)).is_wire() &&
				    cell->getParam(ID(TRANSPARENT)).as_bool()) {
					RTLIL::SigBit clk_bit = sigmap(cell->getPort(ID(CLK)))[0];
					const RTLIL::Memory *memory = module->memories[cell->getParam(ID(MEMID)).decode_string()];
					for (auto memwr_cell : memwr_per_domain[{clk_bit, memory}]) {
						transparent_for[cell].insert(memwr_cell);
						// Our implementation of transparent $memrd cells reads \EN, \ADDR and \DATA from every $memwr cell
						// in the same domain, which isn't directly visible in the netlist. Add these uses explicitly.
						flow.add_uses(memrw_cell_nodes[cell], memwr_cell->getPort(ID(EN)));
						flow.add_uses(memrw_cell_nodes[cell], memwr_cell->getPort(ID(ADDR)));
						flow.add_uses(memrw_cell_nodes[cell], memwr_cell->getPort(ID(DATA)));
					}
				}
			}

			for (auto proc : module->processes) {
				flow.add_node(proc.second);

				for (auto sync : proc.second->syncs)
					switch (sync->type) {
						// Edge-type sync rules require pre-registration.
						case RTLIL::STp:
						case RTLIL::STn:
						case RTLIL::STe:
							register_edge_signal(sigmap, sync->signal, sync->type);
							break;

						// Level-type sync rules require no special handling.
						case RTLIL::ST0:
						case RTLIL::ST1:
						case RTLIL::STa:
							break;

						// Handling of init-type sync rules is delegated to the `proc_init` pass, so we can use the wire
						// attribute regardless of input.
						case RTLIL::STi:
							log_assert(false);

						case RTLIL::STg:
							log_cmd_error("Global clock is not supported.\n");
					}
			}

			for (auto wire : module->wires()) {
				if (!flow.is_elidable(wire)) continue;
				if (wire->port_id != 0) continue;
				if (wire->get_bool_attribute(ID(keep))) continue;
				if (wire->name.begins_with("$") && !elide_internal) continue;
				if (wire->name.begins_with("\\") && !elide_public) continue;
				if (sync_wires[wire]) continue;
				log_assert(flow.wire_defs[wire].size() == 1);
				elided_wires[wire] = **flow.wire_defs[wire].begin();
			}

			// Elided wires that are outputs of internal cells are always connected to a well known port (Y).
			// For user cells, there could be multiple of them, and we need a way to look up the port name
			// knowing only the wire.
			for (auto cell : module->cells())
				for (auto conn : cell->connections())
					if (conn.second.is_wire() && elided_wires.count(conn.second.as_wire()))
						cell_wire_defs[cell][conn.second.as_wire()] = conn.first;

			dict<FlowGraph::Node*, pool<const RTLIL::Wire*>, hash_ptr_ops> node_defs;
			for (auto wire_def : flow.wire_defs)
				for (auto node : wire_def.second)
					node_defs[node].insert(wire_def.first);

			Scheduler<FlowGraph::Node> scheduler;
			dict<FlowGraph::Node*, Scheduler<FlowGraph::Node>::Vertex*, hash_ptr_ops> node_map;
			for (auto node : flow.nodes)
				node_map[node] = scheduler.add(node);
			for (auto node_def : node_defs) {
				auto vertex = node_map[node_def.first];
				for (auto wire : node_def.second)
					for (auto succ_node : flow.wire_uses[wire]) {
						auto succ_vertex = node_map[succ_node];
						vertex->succs.insert(succ_vertex);
						succ_vertex->preds.insert(vertex);
					}
			}

			auto eval_order = scheduler.schedule();
			pool<FlowGraph::Node*, hash_ptr_ops> evaluated;
			pool<const RTLIL::Wire*> feedback_wires;
			for (auto vertex : eval_order) {
				auto node = vertex->data;
				schedule[module].push_back(*node);
				// Any wire that is an output of node vo and input of node vi where vo is scheduled later than vi
				// is a feedback wire. Feedback wires indicate apparent logic loops in the design, which may be
				// caused by a true logic loop, but usually are a benign result of dependency tracking that works
				// on wire, not bit, level. Nevertheless, feedback wires cannot be localized.
				evaluated.insert(node);
				for (auto wire : node_defs[node])
					for (auto succ_node : flow.wire_uses[wire])
						if (evaluated[succ_node]) {
							feedback_wires.insert(wire);
							// Feedback wires may never be elided because feedback requires state, but the point of elision
							// (and localization) is to eliminate state.
							elided_wires.erase(wire);
						}
			}

			if (!feedback_wires.empty()) {
				has_feedback_arcs = true;
				log("Module `%s` contains feedback arcs through wires:\n", module->name.c_str());
				for (auto wire : feedback_wires) {
					log("  %s\n", wire->name.c_str());
				}
			}

			for (auto wire : module->wires()) {
				if (feedback_wires[wire]) continue;
				if (wire->port_id != 0) continue;
				if (wire->get_bool_attribute(ID(keep))) continue;
				if (wire->name.begins_with("$") && !localize_internal) continue;
				if (wire->name.begins_with("\\") && !localize_public) continue;
				if (sync_wires[wire]) continue;
				// Outputs of FF/$memrd cells and LHS of sync actions do not end up in defs.
				if (flow.wire_defs[wire].size() != 1) continue;
				localized_wires.insert(wire);
			}
		}
		if (has_feedback_arcs) {
			log("Feedback arcs require delta cycles during evaluation.\n");
		}
	}

	void check_design(RTLIL::Design *design, bool &has_sync_init, bool &has_packed_mem)
	{
		has_sync_init = has_packed_mem = false;

		for (auto module : design->modules()) {
			if (module->get_blackbox_attribute())
				continue;

			if (!design->selected_whole_module(module))
				if (design->selected_module(module))
					log_cmd_error("Can't handle partially selected module `%s`!\n", id2cstr(module->name));
			if (!design->selected_module(module))
				continue;

			for (auto proc : module->processes)
				for (auto sync : proc.second->syncs)
					if (sync->type == RTLIL::STi)
						has_sync_init = true;

			for (auto cell : module->cells())
				if (cell->type == ID($mem))
					has_packed_mem = true;
		}
	}

	void prepare_design(RTLIL::Design *design)
	{
		bool has_sync_init, has_packed_mem;
		check_design(design, has_sync_init, has_packed_mem);
		if (has_sync_init) {
			// We're only interested in proc_init, but it depends on proc_prune and proc_clean, so call those
			// in case they weren't already. (This allows `yosys foo.v -o foo.cc` to work.)
			Pass::call(design, "proc_prune");
			Pass::call(design, "proc_clean");
			Pass::call(design, "proc_init");
		}
		if (has_packed_mem)
			Pass::call(design, "memory_unpack");
		// Recheck the design if it was modified.
		if (has_sync_init || has_packed_mem)
			check_design(design, has_sync_init, has_packed_mem);
		log_assert(!(has_sync_init || has_packed_mem));

		if (run_splitnets) {
			Pass::call(design, "splitnets -driver");
			Pass::call(design, "opt_clean -purge");
		}
		log("\n");
		analyze_design(design);
	}
};

struct CxxrtlBackend : public Backend {
	static const int DEFAULT_OPT_LEVEL = 5;

	CxxrtlBackend() : Backend("cxxrtl", "convert design to C++ RTL simulation") { }
	void help() YS_OVERRIDE
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    write_cxxrtl [options] [filename]\n");
		log("\n");
		log("Write C++ code for simulating the design. The generated code requires a driver;\n");
		log("the following simple driver is provided as an example:\n");
		log("\n");
		log("    #include \"top.cc\"\n");
		log("\n");
		log("    int main() {\n");
		log("      cxxrtl_design::p_top top;\n");
		log("      while (1) {\n");
		log("        top.p_clk.next = value<1> {1u};\n");
		log("        top.step();\n");
		log("        top.p_clk.next = value<1> {0u};\n");
		log("        top.step();\n");
		log("      }\n");
		log("    }\n");
		log("\n");
		log("The following options are supported by this backend:\n");
		log("\n");
		log("    -header\n");
		log("        generate separate interface (.h) and implementation (.cc) files.\n");
		log("        if specified, the backend must be called with a filename, and filename\n");
		log("        of the interface is derived from filename of the implementation.\n");
		log("        otherwise, interface and implementation are generated together.\n");
		log("\n");
		log("    -namespace <ns-name>\n");
		log("        place the generated code into namespace <ns-name>. if not specified,\n");
		log("        \"cxxrtl_design\" is used.\n");
		log("\n");
		log("    -O <level>\n");
		log("        set the optimization level. the default is -O%d. higher optimization\n", DEFAULT_OPT_LEVEL);
		log("        levels dramatically decrease compile and run time, and highest level\n");
		log("        possible for a design should be used.\n");
		log("\n");
		log("    -O0\n");
		log("        no optimization.\n");
		log("\n");
		log("    -O1\n");
		log("        elide internal wires if possible.\n");
		log("\n");
		log("    -O2\n");
		log("        like -O1, and localize internal wires if possible.\n");
		log("\n");
		log("    -O3\n");
		log("        like -O2, and elide public wires not marked (*keep*) if possible.\n");
		log("\n");
		log("    -O4\n");
		log("        like -O3, and localize public wires not marked (*keep*) if possible.\n");
		log("\n");
		log("    -O5\n");
		log("        like -O4, and run `splitnets -driver; opt_clean -purge` first.\n");
		log("\n");
	}
	void execute(std::ostream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) YS_OVERRIDE
	{
		int opt_level = DEFAULT_OPT_LEVEL;
		CxxrtlWorker worker;

		log_header(design, "Executing CXXRTL backend.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-O" && argidx+1 < args.size()) {
				opt_level = std::stoi(args[++argidx]);
				continue;
			}
			if (args[argidx].substr(0, 2) == "-O" && args[argidx].size() == 3 && isdigit(args[argidx][2])) {
				opt_level = std::stoi(args[argidx].substr(2));
				continue;
			}
			if (args[argidx] == "-header") {
				worker.split_intf = true;
				continue;
			}
			if (args[argidx] == "-namespace" && argidx+1 < args.size()) {
				worker.design_ns = args[++argidx];
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

		switch (opt_level) {
			case 5:
				worker.run_splitnets = true;
			case 4:
				worker.localize_public = true;
			case 3:
				worker.elide_public = true;
			case 2:
				worker.localize_internal = true;
			case 1:
				worker.elide_internal = true;
			case 0:
				break;
			default:
				log_cmd_error("Invalid optimization level %d.\n", opt_level);
		}

		std::ofstream intf_f;
		if (worker.split_intf) {
			if (filename == "<stdout>")
				log_cmd_error("Option -header must be used with a filename.\n");

			worker.intf_filename = filename.substr(0, filename.rfind('.')) + ".h";
			intf_f.open(worker.intf_filename, std::ofstream::trunc);
			if (intf_f.fail())
				log_cmd_error("Can't open file `%s' for writing: %s\n",
				              worker.intf_filename.c_str(), strerror(errno));

			worker.intf_f = &intf_f;
		}
		worker.impl_f = f;

		worker.prepare_design(design);
		worker.dump_design(design);
	}
} CxxrtlBackend;

PRIVATE_NAMESPACE_END
