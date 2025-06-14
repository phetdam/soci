//
// Copyright (C) 2004-2008 Maciej Sobczak, Stephen Hutton
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//

#include "soci/soci.h"
#include "soci/postgresql/soci-postgresql.h"
#include "test-context.h"
#include "test-myint.h"
#include <iostream>
#include <sstream>
#include <string>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdlib>

#include <catch.hpp>

using namespace soci;
using namespace soci::tests;

std::string connectString;
backend_factory const &backEnd = *soci::factory_postgresql();

// Postgres-specific tests

enum TestStringEnum
{
    VALUE_STR_1=0,
    VALUE_STR_2,
    VALUE_STR_3
};

enum TestIntEnum
{
    VALUE_INT_1=0,
    VALUE_INT_2,
    VALUE_INT_3
};

namespace soci {
template <> struct type_conversion<TestStringEnum>
{
    typedef std::string base_type;
    static void from_base(const std::string & v, indicator & ind, TestStringEnum & p)
    {
        if ( ind == i_null )
            throw soci_error("Null value not allowed for this type");

        if ( v.compare("A") == 0 )
            p = TestStringEnum::VALUE_STR_1;
        else if ( v.compare("B") == 0 )
            p = TestStringEnum::VALUE_STR_2;
        else if ( v.compare("C") == 0 )
            p = TestStringEnum::VALUE_STR_3;
        else
            throw soci_error("Value not allowed for this type");
    }
    static void to_base(TestStringEnum & p, std::string & v, indicator & ind)
    {
        switch ( p )
        {
        case TestStringEnum::VALUE_STR_1:
            v = "A";
            ind = i_ok;
            return;
        case TestStringEnum::VALUE_STR_2:
            v = "B";
            ind = i_ok;
            return;
        case TestStringEnum::VALUE_STR_3:
            v = "C";
            ind = i_ok;
            return;
        default:
            throw soci_error("Value not allowed for this type");
        }
    }
};

template <> struct type_conversion<TestIntEnum>
{
    typedef int base_type;
    static void from_base(const int & v, indicator & ind, TestIntEnum & p)
    {
        if ( ind == i_null )
            throw soci_error("Null value not allowed for this type");

        switch( v )
        {
        case 0:
        case 1:
        case 2:
            p = (TestIntEnum)v;
            ind = i_ok;
            return;
        default:
            throw soci_error("Null value not allowed for this type");
        }
    }
    static void to_base(TestIntEnum & p, int & v, indicator & ind)
    {
        switch( p )
        {
        case TestIntEnum::VALUE_INT_1:
        case TestIntEnum::VALUE_INT_2:
        case TestIntEnum::VALUE_INT_3:
            v = (int)p;
            ind = i_ok;
            return;
        default:
            throw soci_error("Value not allowed for this type");
        }
    }
};
}

struct oid_table_creator : public table_creator_base
{
    oid_table_creator(soci::session& sql)
    : table_creator_base(sql)
    {
        sql << "create table soci_test ("
                " id integer,"
                " name varchar(100)"
                ") with oids";
    }
};

TEST_CASE("PostgreSQL connection string", "[postgresql][connstring]")
{
    // There are no required parts in libpq connection string, so we can only
    // test that invalid options are detected.
    CHECK_THROWS_WITH(soci::session(backEnd, "bloordyblop=1"),
                      Catch::Contains(R"(invalid connection option "bloordyblop")"));

    CHECK_THROWS_WITH(soci::session(backEnd, "sslmode=bloordyblop"),
                      Catch::Contains(R"(invalid sslmode value: "bloordyblop")"));

    // This tests that quoted strings work as expected
    CHECK_THROWS_WITH(soci::session(backEnd, "sslmode='dummy value'"),
                      Catch::Contains(R"(invalid sslmode value: "dummy value")"));
    CHECK_THROWS_WITH(soci::session(backEnd, "sslmode=\"dummy value\""),
                      Catch::Contains(R"(invalid sslmode value: "dummy value")"));
}

// ROWID test
// Note: in PostgreSQL, there is no ROWID, there is OID.
// It is still provided as a separate type for "portability",
// whatever that means.
TEST_CASE("PostgreSQL ROWID", "[postgresql][rowid][oid]")
{
    soci::session sql(backEnd, connectString);

    int server_version_num;
    sql << "show server_version_num", into(server_version_num);
    if ( server_version_num >= 120000 )
    {
        WARN("Skipping test because OIDs are no longer supported in PostgreSQL "
             << server_version_num);
        return;
    }

    oid_table_creator tableCreator(sql);

    sql << "insert into soci_test(id, name) values(7, \'John\')";

    rowid rid(sql);
    sql << "select oid from soci_test where id = 7", into(rid);

    int id;
    std::string name;

    sql << "select id, name from soci_test where oid = :rid",
        into(id), into(name), use(rid);

    CHECK(id == 7);
    CHECK(name == "John");
}

TEST_CASE("PostgreSQL prepare error", "[postgresql][exception]")
{
    soci::session sql(backEnd, connectString);

    // Must not cause the application to crash.
    statement st(sql);
    st.prepare(""); // Throws an exception in some versions.
}

// function call test
class function_creator
{
public:
    explicit function_creator(soci::session & sql)
        : sql_(sql)
    {
        drop();

        // before a language can be used it must be defined
        // if it has already been defined then an error will occur
        try { sql << "create language plpgsql"; }
        catch (soci_error const &) {} // ignore if error

        sql  <<
            "create or replace function soci_test(msg varchar) "
            "returns varchar as $$ "
            "declare x int := 1;"
            "begin "
            "  return msg; "
            "end $$ language plpgsql";
    }

    ~function_creator() { drop(); }

private:
    void drop()
    {
        try { sql_ << "drop function soci_test(varchar)"; } catch (soci_error&) {}
    }
    session& sql_;

    SOCI_NOT_COPYABLE(function_creator)
};

TEST_CASE("PostgreSQL function call", "[postgresql][function]")
{
    soci::session sql(backEnd, connectString);

    function_creator functionCreator(sql);

    std::string in("my message");
    std::string out;

    statement st = (sql.prepare <<
        "select soci_test(:input)",
        into(out),
        use(in, "input"));

    st.execute(true);
    CHECK(out == in);

    // explicit procedure syntax
    {
        procedure proc = (sql.prepare <<
            "soci_test(:input)",
            into(out), use(in, "input"));

        proc.execute(true);
        CHECK(out == in);
    }
}

struct longlong_table_creator : table_creator_base
{
    longlong_table_creator(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(val int8)";
    }
};

// long long test
TEST_CASE("PostgreSQL long long", "[postgresql][longlong]")
{
    soci::session sql(backEnd, connectString);

    longlong_table_creator tableCreator(sql);

    long long v1 = 1000000000000LL;
    sql << "insert into soci_test(val) values(:val)", use(v1);

    long long v2 = 0LL;
    sql << "select val from soci_test", into(v2);

    CHECK(v2 == v1);
}

// vector<long long>
TEST_CASE("PostgreSQL vector long long", "[postgresql][vector][longlong]")
{
    soci::session sql(backEnd, connectString);

    longlong_table_creator tableCreator(sql);

    std::vector<long long> v1;
    v1.push_back(1000000000000LL);
    v1.push_back(1000000000001LL);
    v1.push_back(1000000000002LL);
    v1.push_back(1000000000003LL);
    v1.push_back(1000000000004LL);

    sql << "insert into soci_test(val) values(:val)", use(v1);

    std::vector<long long> v2(10);
    sql << "select val from soci_test order by val desc", into(v2);

    REQUIRE(v2.size() == 5);
    CHECK(v2[0] == 1000000000004LL);
    CHECK(v2[1] == 1000000000003LL);
    CHECK(v2[2] == 1000000000002LL);
    CHECK(v2[3] == 1000000000001LL);
    CHECK(v2[4] == 1000000000000LL);
}

// unsigned long long test
TEST_CASE("PostgreSQL unsigned long long", "[postgresql][unsigned][longlong]")
{
    soci::session sql(backEnd, connectString);

    longlong_table_creator tableCreator(sql);

    unsigned long long v1 = 1000000000000ULL;
    sql << "insert into soci_test(val) values(:val)", use(v1);

    unsigned long long v2 = 0ULL;
    sql << "select val from soci_test", into(v2);

    CHECK(v2 == v1);
}

struct boolean_table_creator : table_creator_base
{
    boolean_table_creator(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(val boolean)";
    }
};

TEST_CASE("PostgreSQL boolean", "[postgresql][boolean]")
{
    soci::session sql(backEnd, connectString);

    boolean_table_creator tableCreator(sql);

    int i1 = 0;

    sql << "insert into soci_test(val) values(:val)", use(i1);

    int i2 = 7;
    row r;
    sql << "select val from soci_test", into(i2);
    sql << "select val from soci_test", into(r);

    CHECK(i2 == i1);
    CHECK(r.get<int8_t>(0) == i1);

    sql << "update soci_test set val = true";
    sql << "select val from soci_test", into(i2);
    sql << "select val from soci_test", into(r);
    CHECK(i2 == 1);
    CHECK(r.get<int8_t>(0) == 1);
}

struct uuid_table_creator : table_creator_base
{
    uuid_table_creator(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(val uuid)";
    }
};

// uuid test
TEST_CASE("PostgreSQL uuid", "[postgresql][uuid]")
{
    soci::session sql(backEnd, connectString);

    uuid_table_creator tableCreator(sql);

    std::string v1("cd2dcb78-3817-442e-b12a-17c7e42669a0");
    sql << "insert into soci_test(val) values(:val)", use(v1);

    std::string v2;
    sql << "select val from soci_test", into(v2);

    CHECK(v2 == v1);
}

// dynamic backend test -- currently skipped by default
TEST_CASE("PostgreSQL dynamic backend", "[postgresql][backend][.]")
{
    try
    {
        soci::session sql("nosuchbackend://" + connectString);
        FAIL("expected exception not thrown");
    }
    catch (soci_error const & e)
    {
        CHECK(e.get_error_message() ==
            "Failed to open: libsoci_nosuchbackend.so");
    }

    {
        dynamic_backends::register_backend("pgsql", backEnd);

        std::vector<std::string> backends = dynamic_backends::list_all();
        REQUIRE(backends.size() == 1);
        CHECK(backends[0] == "pgsql");

        {
            soci::session sql("pgsql://" + connectString);
        }

        dynamic_backends::unload("pgsql");

        backends = dynamic_backends::list_all();
        CHECK(backends.empty());
    }

    {
        soci::session sql("postgresql://" + connectString);
    }
}

TEST_CASE("PostgreSQL literals", "[postgresql][into]")
{
    soci::session sql(backEnd, connectString);

    int i;
    sql << "select 123", into(i);
    CHECK(i == 123);

    try
    {
        sql << "select 'ABC'", into (i);
        FAIL("expected exception not thrown");
    }
    catch (soci_error const & e)
    {
        CHECK_THAT( e.what(), Catch::StartsWith("Cannot convert data") );
    }
}

TEST_CASE("PostgreSQL backend name", "[postgresql][backend]")
{
    soci::session sql(backEnd, connectString);

    CHECK(sql.get_backend_name() == "postgresql");
}

// test for double-colon cast in SQL expressions
TEST_CASE("PostgreSQL double colon cast", "[postgresql][cast]")
{
    soci::session sql(backEnd, connectString);

    int a = 123;
    int b = 0;
    sql << "select :a::integer", use(a), into(b);
    CHECK(b == a);
}

// test for date, time and timestamp parsing
TEST_CASE("PostgreSQL datetime", "[postgresql][datetime]")
{
    soci::session sql(backEnd, connectString);

    std::string someDate = "2009-06-17 22:51:03.123";
    std::tm t1 = std::tm(), t2 = std::tm(), t3 = std::tm();

    sql << "select :sd::date, :sd::time, :sd::timestamp",
        use(someDate, "sd"), into(t1), into(t2), into(t3);

    // t1 should contain only the date part
    CHECK(t1.tm_year == 2009 - 1900);
    CHECK(t1.tm_mon == 6 - 1);
    CHECK(t1.tm_mday == 17);
    CHECK(t1.tm_hour == 0);
    CHECK(t1.tm_min == 0);
    CHECK(t1.tm_sec == 0);

    // t2 should contain only the time of day part
    CHECK(t2.tm_year == 0);
    CHECK(t2.tm_mon == 0);
    CHECK(t2.tm_mday == 1);
    CHECK(t2.tm_hour == 22);
    CHECK(t2.tm_min == 51);
    CHECK(t2.tm_sec == 3);

    // t3 should contain all information
    CHECK(t3.tm_year == 2009 - 1900);
    CHECK(t3.tm_mon == 6 - 1);
    CHECK(t3.tm_mday == 17);
    CHECK(t3.tm_hour == 22);
    CHECK(t3.tm_min == 51);
    CHECK(t3.tm_sec == 3);
}

// test for number of affected rows

struct table_creator_for_test11 : table_creator_base
{
    table_creator_for_test11(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(val integer)";
    }
};

TEST_CASE("PostgreSQL get affected rows", "[postgresql][affected-rows]")
{
    soci::session sql(backEnd, connectString);

    table_creator_for_test11 tableCreator(sql);

    for (int i = 0; i != 10; i++)
    {
        sql << "insert into soci_test(val) values(:val)", use(i);
    }

    statement st1 = (sql.prepare <<
        "update soci_test set val = val + 1");
    st1.execute(false);

    CHECK(st1.get_affected_rows() == 10);

    statement st2 = (sql.prepare <<
        "delete from soci_test where val <= 5");
    st2.execute(false);

    CHECK(st2.get_affected_rows() == 5);
}

// test INSERT INTO ... RETURNING syntax

struct table_creator_for_test12 : table_creator_base
{
    table_creator_for_test12(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(sid serial, txt text)";
    }
};

TEST_CASE("PostgreSQL insert into ... returning", "[postgresql]")
{
    soci::session sql(backEnd, connectString);

    table_creator_for_test12 tableCreator(sql);

    std::vector<long> ids(10);
    for (std::size_t i = 0; i != ids.size(); i++)
    {
        long sid(0);
        std::string txt("abc");
        sql << "insert into soci_test(txt) values(:txt) returning sid", use(txt, "txt"), into(sid);
        ids[i] = sid;
    }

    std::vector<long> ids2(ids.size());
    sql << "select sid from soci_test order by sid", into(ids2);
    CHECK(std::equal(ids.begin(), ids.end(), ids2.begin()));
}

struct bytea_table_creator : public table_creator_base
{
    bytea_table_creator(soci::session& sql)
        : table_creator_base(sql)
    {
        sql << "drop table if exists soci_test;";
        sql << "create table soci_test ( val bytea null )";
    }
};

TEST_CASE("PostgreSQL bytea", "[postgresql][bytea]")
{
    soci::session sql(backEnd, connectString);

    // PostgreSQL supports two different output formats for bytea values:
    // historical "escape" format, which is the only one supported until
    // PostgreSQL 9.0, and "hex" format used by default since 9.0, we need
    // to determine which one is actually in use.
    std::string bytea_output_format;
    sql << "select setting from pg_settings where name='bytea_output'",
           into(bytea_output_format);
    char const* expectedBytea;
    if (bytea_output_format.empty() || bytea_output_format == "escape")
      expectedBytea = "\\015\\014\\013\\012";
    else if (bytea_output_format == "hex")
      expectedBytea = "\\x0d0c0b0a";
    else
      throw std::runtime_error("Unknown PostgreSQL bytea_output \"" +
                               bytea_output_format + "\"");

    bytea_table_creator tableCreator(sql);

    int v = 0x0A0B0C0D;
    unsigned char* b = reinterpret_cast<unsigned char*>(&v);
    std::string data;
    std::copy(b, b + sizeof(v), std::back_inserter(data));
    {

        sql << "insert into soci_test(val) values(:val)", use(data);

        // 1) into string, no Oid mapping
        std::string bin1;
        sql << "select val from soci_test", into(bin1);
        CHECK(bin1 == expectedBytea);

        // 2) Oid-to-dt_string mapped
        row r;
        sql << "select * from soci_test", into(r);

        REQUIRE(r.size() == 1);
        column_properties const& props = r.get_properties(0);
        CHECK(props.get_data_type() == soci::dt_string);
        CHECK(props.get_db_type() == soci::db_string);
        std::string bin2 = r.get<std::string>(0);
        CHECK(bin2 == expectedBytea);
    }
}

// json
struct table_creator_json : public table_creator_base
{
    table_creator_json(soci::session& sql)
    : table_creator_base(sql)
    {
        sql << "drop table if exists soci_json_test;";
        sql << "create table soci_json_test(data json)";
    }
};

// Return 9,2 for 9.2.3
typedef std::pair<int,int> server_version;

server_version get_postgresql_version(soci::session& sql)
{
    std::string version;
    std::pair<int,int> result;
    sql << "select version()",into(version);
    if (soci::sscanf(version.c_str(),"PostgreSQL %i.%i", &result.first, &result.second) < 2)
    {
        throw std::runtime_error("Failed to retrieve PostgreSQL version number");
    }
    return result;
}

// Test JSON. Only valid for PostgreSQL Server 9.2++
TEST_CASE("PostgreSQL JSON", "[postgresql][json]")
{
    soci::session sql(backEnd, connectString);
    server_version version = get_postgresql_version(sql);
    if ( version >= server_version(9,2))
    {
        std::string result;
        std::string valid_input = "{\"tool\":\"soci\",\"result\":42}";
        std::string invalid_input = "{\"tool\":\"other\",\"result\":invalid}";

        table_creator_json tableCreator(sql);

        sql << "insert into soci_json_test (data) values(:data)",use(valid_input);
        sql << "select data from  soci_json_test",into(result);
        CHECK(result == valid_input);

        CHECK_THROWS_AS((
            sql << "insert into soci_json_test (data) values(:data)",use(invalid_input)),
            soci_error
        );
    }
    else
    {
        WARN("JSON test skipped (PostgreSQL >= 9.2 required, found " << version.first << "." << version.second << ")");
    }
}

struct table_creator_text : public table_creator_base
{
    table_creator_text(soci::session& sql) : table_creator_base(sql)
    {
        sql << "drop table if exists soci_test;";
        sql << "create table soci_test(name varchar(20))";
    }
};

// Test deallocate_prepared_statement called for non-existing statement
// which creation failed due to invalid SQL syntax.
// https://github.com/SOCI/soci/issues/116
TEST_CASE("PostgreSQL statement prepare failure", "[postgresql][prepare]")
{
    soci::session sql(backEnd, connectString);
    table_creator_text tableCreator(sql);

    try
    {
        // types mismatch should lead to PQprepare failure
        statement get_trades =
            (sql.prepare
                << "select * from soci_test where name=9999");
        FAIL("expected exception not thrown");
    }
    catch (postgresql_soci_error const& e)
    {
        CHECK( e.get_error_category() == soci_error::invalid_statement );

        CHECK_THAT( e.what(), Catch::Contains("operator does not exist") );
    }
}

// Test the support of PostgreSQL-style casts with ORM
TEST_CASE("PostgreSQL ORM cast", "[postgresql][orm]")
{
    soci::session sql(backEnd, connectString);
    values v;
    v.set("a", 1);
    sql << "select :a::int", use(v); // Must not throw an exception!
}

std::string get_table_name_without_schema(const std::string& table_name_with_schema)
{
    // Find the first occurrence of "."
    size_t dotPos = table_name_with_schema.find('.');

    // Check if the "." exists and there's exactly one "."
    if (dotPos == std::string::npos || table_name_with_schema.find('.', dotPos + 1) != std::string::npos) {
        return table_name_with_schema;
    }

    // Extract the substring after the "."
    return table_name_with_schema.substr(dotPos + 1);
}

std::string get_schema_from_table_name(const std::string& table_name_with_schema)
{
    // Find the first occurrence of "."
    size_t dotPos = table_name_with_schema.find('.');

    // Check if the "." exists and there's exactly one "."
    if (dotPos == std::string::npos || table_name_with_schema.find('.', dotPos + 1) != std::string::npos) {
        return "";
    }

    // Extract the substring before the "."
    return table_name_with_schema.substr(0, dotPos);
}

// Test the DDL and metadata functionality
TEST_CASE("PostgreSQL DDL with metadata", "[postgresql][ddl]")
{
    soci::session sql(backEnd, connectString);

    // note: prepare_column_descriptions expects l-value
    std::string ddl_t1 = "ddl_t1";
    std::string ddl_t2 = "ddl_t2";
    std::string ddl_t3 = "ddl_t3";

    // single-expression variant:
    sql.create_table(ddl_t1).column("i", soci::dt_integer).column("j", soci::dt_integer);

    // check whether this table was created:

    bool ddl_t1_found = false;
    bool ddl_t2_found = false;
    bool ddl_t3_found = false;
    std::string table_name;
    soci::statement st = (sql.prepare_table_names(), into(table_name));
    st.execute();
    while (st.fetch())
    {
        if (get_table_name_without_schema(table_name) == ddl_t1) { ddl_t1_found = true; }
        if (get_table_name_without_schema(table_name) == ddl_t2) { ddl_t2_found = true; }
        if (get_table_name_without_schema(table_name) == ddl_t3) { ddl_t3_found = true; }
    }

    CHECK(ddl_t1_found);
    CHECK(ddl_t2_found == false);
    CHECK(ddl_t3_found == false);

    // check whether ddl_t1 has the right structure:

    bool i_found = false;
    bool j_found = false;
    bool other_found = false;
    soci::column_info ci;
    soci::statement st1 = (sql.prepare_column_descriptions(ddl_t1), into(ci));
    st1.execute();
    while (st1.fetch())
    {
        if (ci.name == "i")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable);
            i_found = true;
        }
        else if (ci.name == "j")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable);
            j_found = true;
        }
        else
        {
            other_found = true;
        }
    }

    CHECK(i_found);
    CHECK(j_found);
    CHECK(other_found == false);

    // two more tables:

    // separately defined columns:
    // (note: statement is executed when ddl object goes out of scope)
    {
        soci::ddl_type ddl = sql.create_table(ddl_t2);
        ddl.column("i", soci::dt_integer);
        ddl.column("j", soci::dt_integer);
        ddl.column("k", soci::dt_integer)("not null");
        ddl.primary_key("t2_pk", "j");
    }

    sql.add_column(ddl_t1, "k", soci::dt_integer);
    sql.add_column(ddl_t1, "big", soci::dt_string, 0); // "unlimited" length -> text
    sql.drop_column(ddl_t1, "i");

    // or with constraint as in t2:
    sql.add_column(ddl_t2, "m", soci::dt_integer)("not null");

    // third table with a foreign key to the second one
    {
        soci::ddl_type ddl = sql.create_table(ddl_t3);
        ddl.column("x", soci::dt_integer);
        ddl.column("y", soci::dt_integer);
        ddl.foreign_key("t3_fk", "x", ddl_t2, "j");
    }

    // check if all tables were created:

    ddl_t1_found = false;
    ddl_t2_found = false;
    ddl_t3_found = false;
    soci::statement st2 = (sql.prepare_table_names(), into(table_name));
    st2.execute();
    while (st2.fetch())
    {
        if (get_table_name_without_schema(table_name) == ddl_t1) { ddl_t1_found = true; }
        if (get_table_name_without_schema(table_name) == ddl_t2) { ddl_t2_found = true; }
        if (get_table_name_without_schema(table_name) == ddl_t3) { ddl_t3_found = true; }
    }

    CHECK(ddl_t1_found);
    CHECK(ddl_t2_found);
    CHECK(ddl_t3_found);

    // check if ddl_t1 has the right structure (it was altered):

    i_found = false;
    j_found = false;
    bool k_found = false;
    bool big_found = false;
    other_found = false;
    soci::statement st3 = (sql.prepare_column_descriptions(ddl_t1), into(ci));
    st3.execute();
    while (st3.fetch())
    {
        if (ci.name == "j")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable);
            j_found = true;
        }
        else if (ci.name == "k")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable);
            k_found = true;
        }
        else if (ci.name == "big")
        {
            CHECK(ci.type == soci::dt_string);
            CHECK(ci.dataType == soci::db_string);
            CHECK(ci.precision == 0); // "unlimited" for strings
            big_found = true;
        }
        else
        {
            other_found = true;
        }
    }

    CHECK(i_found == false);
    CHECK(j_found);
    CHECK(k_found);
    CHECK(big_found);
    CHECK(other_found == false);

    // check if ddl_t2 has the right structure:

    i_found = false;
    j_found = false;
    k_found = false;
    bool m_found = false;
    other_found = false;
    soci::statement st4 = (sql.prepare_column_descriptions(ddl_t2), into(ci));
    st4.execute();
    while (st4.fetch())
    {
        if (ci.name == "i")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable);
            i_found = true;
        }
        else if (ci.name == "j")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable == false); // primary key
            j_found = true;
        }
        else if (ci.name == "k")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable == false);
            k_found = true;
        }
        else if (ci.name == "m")
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable == false);
            m_found = true;
        }
        else
        {
            other_found = true;
        }
    }

    CHECK(i_found);
    CHECK(j_found);
    CHECK(k_found);
    CHECK(m_found);
    CHECK(other_found == false);

    sql.drop_table(ddl_t1);
    sql.drop_table(ddl_t3); // note: this must be dropped before ddl_t2
    sql.drop_table(ddl_t2);

    // check if all tables were dropped:

    ddl_t1_found = false;
    ddl_t2_found = false;
    ddl_t3_found = false;
    st2 = (sql.prepare_table_names(), into(table_name));
    st2.execute();
    while (st2.fetch())
    {
        if (get_table_name_without_schema(table_name) == ddl_t1) { ddl_t1_found = true; }
        if (get_table_name_without_schema(table_name) == ddl_t2) { ddl_t2_found = true; }
        if (get_table_name_without_schema(table_name) == ddl_t3) { ddl_t3_found = true; }
    }

    CHECK(ddl_t1_found == false);
    CHECK(ddl_t2_found == false);
    CHECK(ddl_t3_found == false);

    int i = -1;
    sql << "select lo_unlink(" + sql.empty_blob() + ")", into(i);
    CHECK(i == 1);
    sql << "select " + sql.nvl() + "(1, 2)", into(i);
    CHECK(i == 1);
    sql << "select " + sql.nvl() + "(NULL, 2)", into(i);
    CHECK(i == 2);
}

// Test cross-schema metadata
TEST_CASE("Cross-schema metadata", "[postgresql][cross-schema]")
{
    soci::session sql(backEnd, connectString);

    // note: prepare_column_descriptions expects l-value
    std::string tables = "tables";
    std::string column_name = "table_name";

    // single-expression variant:
    sql.create_table(tables).column(column_name, soci::dt_integer);

    // check whether this table was created:

    bool tables_found = false;
    std::string schema;
    std::string table_name;
    soci::statement st = (sql.prepare_table_names(), into(table_name));
    st.execute();
    while (st.fetch())
    {
        if (get_table_name_without_schema(table_name) == tables) 
        {
            tables_found = true;
            schema = get_schema_from_table_name(table_name);
        }
    }

    CHECK(tables_found);
    CHECK(!schema.empty());

    // Get information for the tables table we just created and not
    // the tables table in information_schema which isn't in our path.
    int  records = 0;
    soci::column_info ci;
    soci::statement st1 = (sql.prepare_column_descriptions(tables), into(ci));
    st1.execute();
    while (st1.fetch())
    {
        if (ci.name == column_name)
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable);
            records++;
        }
    }

    CHECK(records == 1);

    // Run the same query but this time specific with the schema.
    std::string schemaTables = schema + "." + tables;
    records = 0;
    soci::statement st2 = (sql.prepare_column_descriptions(schemaTables), into(ci));
    st2.execute();
    while (st2.fetch())
    {
        if (ci.name == column_name)
        {
            CHECK(ci.type == soci::dt_integer);
            CHECK(ci.dataType == soci::db_int32);
            CHECK(ci.nullable);
            records++;
        }
    }

    CHECK(records == 1);

    // Finally run the query with the information_schema.
    std::string information_schemaTables = "information_schema." + tables;
    records = 0;
    soci::statement st3 = (sql.prepare_column_descriptions(information_schemaTables), into(ci));
    st3.execute();
    while (st3.fetch())
    {
        if (ci.name == column_name)
        {
            CHECK(ci.type == soci::dt_string);
            CHECK(ci.dataType == soci::db_string);
            CHECK(ci.nullable);
            records++;
        }
    }

    CHECK(records == 1);

    // Delete table and check that it is gone
    sql.drop_table(tables);
    tables_found = false;
    st3 = (sql.prepare_table_names(), into(table_name));
    st3.execute();
    while (st.fetch())
    {
        if (table_name == tables)
        {
            tables_found = true;
        }
    }

    CHECK(tables_found == false);
}

// Test the bulk iterators functionality
TEST_CASE("Bulk iterators", "[postgresql][bulkiters]")
{
    soci::session sql(backEnd, connectString);

    sql << "create table t (i integer)";

    // test bulk iterators with basic types
    {
        std::vector<int> v;
        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        v.push_back(40);
        v.push_back(50);

        std::size_t begin = 2;
        std::size_t end = 5;
        sql << "insert into t (i) values (:v)", soci::use(v, begin, end);

        v.clear();
        v.resize(20);
        begin = 5;
        end = 20;
        sql << "select i from t", soci::into(v, begin, end);

        CHECK(end == 8);
        for (std::size_t i = 0; i != 5; ++i)
        {
            CHECK(v[i] == 0);
        }
        CHECK(v[5] == 30);
        CHECK(v[6] == 40);
        CHECK(v[7] == 50);
        for (std::size_t i = end; i != 20; ++i)
        {
            CHECK(v[i] == 0);
        }
    }

    sql << "delete from t";

    // test bulk iterators with user types
    {
        std::vector<MyInt> v;
        v.push_back(MyInt(10));
        v.push_back(MyInt(20));
        v.push_back(MyInt(30));
        v.push_back(MyInt(40));
        v.push_back(MyInt(50));

        std::size_t begin = 2;
        std::size_t end = 5;
        sql << "insert into t (i) values (:v)", soci::use(v, begin, end);

        v.clear();
        for (std::size_t i = 0; i != 20; ++i)
        {
            v.push_back(MyInt(-1));
        }

        begin = 5;
        end = 20;
        sql << "select i from t", soci::into(v, begin, end);

        CHECK(end == 8);
        for (std::size_t i = 0; i != 5; ++i)
        {
            CHECK(v[i].get() == -1);
        }
        CHECK(v[5].get() == 30);
        CHECK(v[6].get() == 40);
        CHECK(v[7].get() == 50);
        for (std::size_t i = end; i != 20; ++i)
        {
            CHECK(v[i].get() == -1);
        }
    }

    sql << "drop table t";
}


// false_bind_variable_inside_identifier
struct test_false_bind_variable_inside_identifier_table_creator : table_creator_base
{
    test_false_bind_variable_inside_identifier_table_creator(soci::session & sql)
        : table_creator_base(sql)
        , msession(sql)
    {

        try
        {
            sql << "CREATE TABLE soci_test( \"column_with:colon\" integer)";
            sql << "CREATE TYPE \"type_with:colon\" AS ENUM ('en_one', 'en_two');";
            sql <<  "CREATE FUNCTION \"function_with:colon\"() RETURNS integer LANGUAGE 'sql' AS "
                    "$BODY$"
                    "   SELECT \"column_with:colon\" FROM soci_test LIMIT 1; "
                    "$BODY$;"
            ;
        }
        catch(...)
        {
            drop();
        }

    }
    ~test_false_bind_variable_inside_identifier_table_creator(){
        drop();
    }
private:
    void drop()
    {
        try
        {
            msession << "DROP FUNCTION IF EXISTS \"function_with:colon\"();";
            msession << "DROP TYPE IF EXISTS \"type_with:colon\" ;";
        }
        catch (soci_error const&){}
    }
    soci::session& msession;
};
TEST_CASE("false_bind_variable_inside_identifier", "[postgresql][bind-variables]")
{
    std::string col_name;
    int fct_return_value;
    std::string type_value;

    {
        soci::session sql(backEnd, connectString);
        test_false_bind_variable_inside_identifier_table_creator tableCreator(sql);

        sql << "insert into soci_test(\"column_with:colon\") values(2020)";
        sql << "SELECT column_name FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'soci_test';", into(col_name);
        sql << "SELECT \"function_with:colon\"() ;", into(fct_return_value);
        sql << "SELECT unnest(enum_range(NULL::\"type_with:colon\"))  ORDER BY 1 LIMIT 1;", into(type_value);
    }

    CHECK(col_name.compare("column_with:colon") == 0);
    CHECK(fct_return_value == 2020);
    CHECK(type_value.compare("en_one")==0);
}

// test_enum_with_explicit_custom_type_string_rowset
struct test_enum_with_explicit_custom_type_string_rowset : table_creator_base
{
    test_enum_with_explicit_custom_type_string_rowset(soci::session & sql)
        : table_creator_base(sql)
        , msession(sql)
    {
        try
        {
            sql << "CREATE TYPE EnumType AS ENUM ('A','B','C');";
            sql << "CREATE TABLE soci_test (Type EnumType NOT NULL DEFAULT 'A');";
        }
        catch (...)
        {
            drop();
        }

    }
    ~test_enum_with_explicit_custom_type_string_rowset()
    {
        drop();
    }

private:
    void drop()
    {
        try
        {
            msession << "drop table if exists soci_test;";
            msession << "DROP TYPE IF EXISTS EnumType ;";
        }
        catch (soci_error const& e){
            std::cerr << e.what() << std::endl;
        }
    }
    soci::session& msession;
};

TEST_CASE("test_enum_with_explicit_custom_type_string_rowset", "[postgresql][bind-variables]")
{
    TestStringEnum test_value = TestStringEnum::VALUE_STR_2;
    TestStringEnum type_value;

    {
        soci::session sql(backEnd, connectString);
        test_enum_with_explicit_custom_type_string_rowset tableCreator(sql);

        statement s1 = (sql.prepare << "insert into soci_test values(:val);", use(test_value, "val"));
        statement s2 = (sql.prepare << "SELECT Type FROM soci_test;");

        s1.execute(false);

        soci::row result;
        s2.define_and_bind();
        s2.exchange_for_rowset(soci::into(result));
        s2.execute(true);

        type_value = result.get<TestStringEnum>("type");
    }

    CHECK(type_value==TestStringEnum::VALUE_STR_2);
}

TEST_CASE("test_enum_with_explicit_custom_type_string_into", "[postgresql][bind-variables]")
{
    TestStringEnum test_value = TestStringEnum::VALUE_STR_2;
    TestStringEnum type_value;

    {
        soci::session sql(backEnd, connectString);
        test_enum_with_explicit_custom_type_string_rowset tableCreator(sql);

        statement s1 = (sql.prepare << "insert into soci_test values(:val);", use(test_value, "val"));
        statement s2 = (sql.prepare << "SELECT Type FROM soci_test;", into(type_value));

        s1.execute(false);
        s2.execute(true);
    }

    CHECK(type_value==TestStringEnum::VALUE_STR_2);
}

// test_enum_with_explicit_custom_type_int_rowset
struct test_enum_with_explicit_custom_type_int_rowset : table_creator_base
{
    test_enum_with_explicit_custom_type_int_rowset(soci::session & sql)
        : table_creator_base(sql)
        , msession(sql)
    {

        try
        {
            sql << "CREATE TABLE soci_test( Type smallint)";
            ;
        }
        catch (...)
        {
            drop();
        }

    }
    ~test_enum_with_explicit_custom_type_int_rowset()
    {
        drop();
    }

private:
    void drop()
    {
        try
        {
            msession << "drop table if exists soci_test;";
        }
        catch (soci_error const& e){
            std::cerr << e.what() << std::endl;
        }
    }
    soci::session& msession;
};

TEST_CASE("test_enum_with_explicit_custom_type_int_rowset", "[postgresql][bind-variables]")
{
    TestIntEnum test_value = TestIntEnum::VALUE_INT_2;
    TestIntEnum type_value;

    {
        soci::session sql(backEnd, connectString);
        test_enum_with_explicit_custom_type_int_rowset tableCreator(sql);

        statement s1 = (sql.prepare << "insert into soci_test(Type) values(:val)", use(test_value, "val"));
        statement s2 = (sql.prepare << "SELECT Type FROM soci_test ;");

        s1.execute(false);

        soci::row result;
        s2.define_and_bind();
        s2.exchange_for_rowset(soci::into(result));
        s2.execute(true);

        type_value = result.get<TestIntEnum>("type");
    }

    CHECK(type_value==TestIntEnum::VALUE_INT_2);
}

TEST_CASE("test_enum_with_explicit_custom_type_int_into", "[postgresql][bind-variables]")
{
    TestIntEnum test_value = TestIntEnum::VALUE_INT_2;
    TestIntEnum type_value;

    {
        soci::session sql(backEnd, connectString);
        test_enum_with_explicit_custom_type_int_rowset tableCreator(sql);

        statement s1 = (sql.prepare << "insert into soci_test(Type) values(:val)", use(test_value, "val"));
        statement s2 = (sql.prepare << "SELECT Type FROM soci_test ;", into(type_value));

        s1.execute(false);
        s2.execute(true);
    }

    CHECK(type_value==TestIntEnum::VALUE_INT_2);
}

// false_bind_variable_inside_identifier
struct table_creator_colon_in_double_quotes_in_single_quotes :
    table_creator_base
{
    table_creator_colon_in_double_quotes_in_single_quotes(soci::session & sql)
        : table_creator_base(sql)
    {
       sql << "CREATE TABLE soci_test( \"column_with:colon\" text)";
    }

};
TEST_CASE("colon_in_double_quotes_in_single_quotes",
          "[postgresql][bind-variables]")
{
    std::string return_value;

    {
        soci::session sql(backEnd, connectString);
        table_creator_colon_in_double_quotes_in_single_quotes
            tableCreator(sql);

        sql << "insert into soci_test(\"column_with:colon\") values('hello "
               "it is \"10:10\"')";
        sql << "SELECT \"column_with:colon\" from soci_test ;", into
            (return_value);
    }

    CHECK(return_value == "hello it is \"10:10\"");
}

//
// Support for soci Common Tests
//

// DDL Creation objects for common tests
struct table_creator_one : public table_creator_base
{
    table_creator_one(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(id integer, val integer, c char, "
                 "str varchar(20), sh int2, ll bigint, ul numeric(20), "
                 "d float8, num76 numeric(7,6), "
                 "tm timestamp, i1 integer, i2 integer, i3 integer, "
                 "name varchar(20))";
    }
};

struct table_creator_two : public table_creator_base
{
    table_creator_two(soci::session & sql)
        : table_creator_base(sql)
    {
        sql  << "create table soci_test(num_float float8, num_int integer,"
                     " name varchar(20), sometime timestamp, chr char)";
    }
};

struct table_creator_three : public table_creator_base
{
    table_creator_three(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(name varchar(100) not null, "
            "phone varchar(15))";
    }
};

struct table_creator_for_get_affected_rows : table_creator_base
{
    table_creator_for_get_affected_rows(soci::session & sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(val integer)";
    }
};

struct table_creator_for_xml : table_creator_base
{
    table_creator_for_xml(soci::session& sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(id integer, x xml)";
    }
};

struct table_creator_for_clob : table_creator_base
{
    table_creator_for_clob(soci::session& sql)
        : table_creator_base(sql)
    {
        sql << "create table soci_test(id integer, s text)";
    }
};

struct table_creator_for_blob : public tests::table_creator_base
{
    table_creator_for_blob(soci::session & sql)
		: tests::table_creator_base(sql)
    {
        sql << "create table soci_test(id integer, b oid)";
    }
};


class test_context : public test_context_common
{
public:
    test_context() = default;

    std::string get_example_connection_string() const override
    {
        return "host=localhost port=5432 dbname=test user=postgres password=postgres";
    }

    std::string get_backend_name() const override
    {
        return "postgresql";
    }

    table_creator_base* table_creator_1(soci::session& s) const override
    {
        return new table_creator_one(s);
    }

    table_creator_base* table_creator_2(soci::session& s) const override
    {
        return new table_creator_two(s);
    }

    table_creator_base* table_creator_3(soci::session& s) const override
    {
        return new table_creator_three(s);
    }

    table_creator_base* table_creator_4(soci::session& s) const override
    {
        return new table_creator_for_get_affected_rows(s);
    }

    table_creator_base* table_creator_xml(soci::session& s) const override
    {
        return new table_creator_for_xml(s);
    }

    table_creator_base* table_creator_clob(soci::session& s) const override
    {
        return new table_creator_for_clob(s);
    }

    table_creator_base* table_creator_blob(soci::session& s) const override
    {
        return new table_creator_for_blob(s);
    }

    bool has_real_xml_support() const override
    {
        return true;
    }

    std::string to_date_time(std::string const &datdt_string) const override
    {
        return "timestamptz(\'" + datdt_string + "\')";
    }

    bool has_fp_bug() const override
    {
        return false;
    }

    std::string sql_length(std::string const& s) const override
    {
        return "char_length(" + s + ")";
    }
};

test_context tc_postgresql;
