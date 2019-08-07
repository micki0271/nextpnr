/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <miodrag@symbioticeda.com>
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

#include "jsonwrite.h"
#include <assert.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <log.h>
#include <map>
#include <string>
#include "nextpnr.h"
#include "version.h"

NEXTPNR_NAMESPACE_BEGIN

namespace JsonWriter {

std::string get_string(std::string str)
{
    std::string newstr = "\"";
    for (char c : str) {
        if (c == '\\')
            newstr += c;
        newstr += c;
    }
    return newstr + "\"";
}

std::string get_name(IdString name, Context *ctx) { return get_string(name.c_str(ctx)); }

void write_parameter_value(std::ostream &f, const Property &value)
{
    if (value.size() == 32 && value.is_fully_def()) {
        f << stringf("%d", value.as_int64());
    } else {
        f << get_string(value.to_string());
    }
}

void write_parameters(std::ostream &f, Context *ctx, const std::unordered_map<IdString, Property> &parameters,
                      bool for_module = false)
{
    bool first = true;
    for (auto &param : parameters) {
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s%s: ", for_module ? "" : "    ", get_name(param.first, ctx).c_str());
        write_parameter_value(f, param.second);
        first = false;
    }
}

struct PortGroup
{
    std::string name;
    std::vector<int> bits;
    PortType dir;
};

std::vector<PortGroup> group_ports(Context *ctx)
{
    std::vector<PortGroup> groups;
    std::unordered_map<std::string, size_t> base_to_group;
    for (auto &pair : ctx->ports) {
        std::string name = pair.second.name.str(ctx);
        if ((name.back() != ']') || (name.find('[') == std::string::npos)) {
            groups.push_back({name, {pair.first.index}, pair.second.type});
        } else {
            int off1 = int(name.find_last_of('['));
            std::string basename = name.substr(0, off1);
            int index = std::stoi(name.substr(off1 + 1, name.size() - (off1 + 2)));

            if (!base_to_group.count(basename)) {
                base_to_group[basename] = groups.size();
                groups.push_back({basename, std::vector<int>(index + 1, -1), pair.second.type});
            }

            auto &grp = groups.at(base_to_group[basename]);
            if (int(grp.bits.size()) <= index)
                grp.bits.resize(index + 1, -1);
            NPNR_ASSERT(grp.bits.at(index) == -1);
            grp.bits.at(index) = pair.first.index;
        }
    }
    return groups;
};

std::string format_port_bits(const PortGroup &port)
{
    std::stringstream s;
    s << "[ ";
    bool first = true;
    for (auto bit : port.bits) {
        if (!first)
            s << ", ";
        if (bit == -1)
            s << "\"x\"";
        else
            s << bit;
        first = false;
    }
    s << " ]";
    return s.str();
}

void write_module(std::ostream &f, Context *ctx)
{
    auto val = ctx->attrs.find(ctx->id("module"));
    if (val != ctx->attrs.end())
        f << stringf("    %s: {\n", get_string(val->second.as_string()).c_str());
    else
        f << stringf("    %s: {\n", get_string("top").c_str());
    f << stringf("      \"settings\": {");
    write_parameters(f, ctx, ctx->settings, true);
    f << stringf("\n      },\n");
    f << stringf("      \"attributes\": {");
    write_parameters(f, ctx, ctx->attrs, true);
    f << stringf("\n      },\n");
    f << stringf("      \"ports\": {");

    auto ports = group_ports(ctx);
    bool first = true;
    for (auto &port : ports) {
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s: {\n", get_string(port.name).c_str());
        f << stringf("          \"direction\": \"%s\",\n",
                     port.dir == PORT_IN ? "input" : port.dir == PORT_INOUT ? "inout" : "output");
        f << stringf("          \"bits\": %s\n", format_port_bits(port).c_str());
        f << stringf("        }");
        first = false;
    }
    f << stringf("\n      },\n");

    f << stringf("      \"cells\": {");
    first = true;
    for (auto &pair : ctx->cells) {
        auto &c = pair.second;
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s: {\n", get_name(c->name, ctx).c_str());
        f << stringf("          \"hide_name\": %s,\n", c->name.c_str(ctx)[0] == '$' ? "1" : "0");
        f << stringf("          \"type\": %s,\n", get_name(c->type, ctx).c_str());
        f << stringf("          \"parameters\": {");
        write_parameters(f, ctx, c->params);
        f << stringf("\n          },\n");
        f << stringf("          \"attributes\": {");
        write_parameters(f, ctx, c->attrs);
        f << stringf("\n          },\n");
        f << stringf("          \"port_directions\": {");
        bool first2 = true;
        for (auto &conn : c->ports) {
            auto &p = conn.second;
            std::string direction = (p.type == PORT_IN) ? "input" : (p.type == PORT_OUT) ? "output" : "inout";
            f << stringf("%s\n", first2 ? "" : ",");
            f << stringf("            %s: \"%s\"", get_name(conn.first, ctx).c_str(), direction.c_str());
            first2 = false;
        }
        f << stringf("\n          },\n");
        f << stringf("          \"connections\": {");
        first2 = true;
        for (auto &conn : c->ports) {
            auto &p = conn.second;
            f << stringf("%s\n", first2 ? "" : ",");
            if (p.net)
                f << stringf("            %s: [ %d ]", get_name(conn.first, ctx).c_str(), p.net->name.index);
            else
                f << stringf("            %s: [ ]", get_name(conn.first, ctx).c_str());

            first2 = false;
        }
        f << stringf("\n          }\n");

        f << stringf("        }");
        first = false;
    }

    f << stringf("\n      },\n");

    f << stringf("      \"netnames\": {");
    first = true;
    for (auto &pair : ctx->nets) {
        auto &w = pair.second;
        f << stringf("%s\n", first ? "" : ",");
        f << stringf("        %s: {\n", get_name(w->name, ctx).c_str());
        f << stringf("          \"hide_name\": %s,\n", w->name.c_str(ctx)[0] == '$' ? "1" : "0");
        f << stringf("          \"bits\": [ %d ] ,\n", pair.first.index);
        f << stringf("          \"attributes\": {");
        write_parameters(f, ctx, w->attrs);
        f << stringf("\n          }\n");
        f << stringf("        }");
        first = false;
    }

    f << stringf("\n      }\n");
    f << stringf("    }");
}

void write_context(std::ostream &f, Context *ctx)
{
    f << stringf("{\n");
    f << stringf("  \"creator\": %s,\n",
                 get_string("Next Generation Place and Route (git sha1 " GIT_COMMIT_HASH_STR ")").c_str());
    f << stringf("  \"modules\": {\n");
    write_module(f, ctx);
    f << stringf("\n  }");
    f << stringf("\n}\n");
}

}; // End Namespace JsonWriter

bool write_json_file(std::ostream &f, std::string &filename, Context *ctx)
{
    try {
        using namespace JsonWriter;
        if (!f)
            log_error("failed to open JSON file.\n");
        write_context(f, ctx);
        log_break();
        return true;
    } catch (log_execution_error_exception) {
        return false;
    }
}

NEXTPNR_NAMESPACE_END
