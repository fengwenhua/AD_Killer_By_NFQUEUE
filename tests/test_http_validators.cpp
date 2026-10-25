#include "http_validators.h"
#include <cassert>
#include <iostream>

int main() {
    assert(valid_etag("\"a,b\"") && valid_etag("W/\"abc\"") && valid_etag("\"\""));
    assert(!valid_etag("*") && !valid_etag("abc") && !valid_etag("\"a b\"") && !valid_etag("\"a\"b\""));
    assert(!valid_http_validators({"\"ok\"\r\nX:bad", ""}));
    assert(!valid_http_validators({"", "x\nInjected: true"}));
    HttpResponseHeaders headers;
    assert(headers.consume("HTTP/1.1 301 Moved\r\n") && headers.consume("ETag: \"redirect\"\r\n") && headers.consume("\r\n"));
    assert(headers.consume("HTTP/2 200\r\n") && headers.consume("ETag: W/\"final\"\r\n"));
    assert(headers.consume("Last-Modified: Wed, 21 Oct 2015 07:28:00 GMT\r\n") && headers.consume("\r\n"));
    assert(headers.valid() && headers.status() == 200 && headers.validators().etag == "W/\"final\"");
    assert(headers.consume("ETag: \"trailer\"\r\n") && headers.validators().etag == "W/\"final\"");
    assert(headers.consume("HTTP/1.1 304 Not Modified\r\n") && headers.consume("\r\n"));
    assert(headers.valid() && headers.status() == 304 && headers.validators().empty());
    HttpResponseHeaders duplicate;
    for (const char *line : {"HTTP/1.1 200 OK\n", "ETag: \"one\"\n", "ETag: \"two\"\n", "ETag: \"three\"\n", "\n"}) assert(duplicate.consume(line));
    assert(duplicate.valid() && duplicate.validators().empty());
    HttpResponseHeaders large;
    assert(large.consume("HTTP/1.1 200 OK\r\n"));
    assert(!large.consume(std::string(HttpResponseHeaders::MAX_BYTES, 'x') + "\r\n") && !large.valid());
    HttpResponseHeaders malformed;
    assert(!malformed.consume("HTTP/1.1 2000 Bad\r\n"));
    HttpResponseHeaders folded;
    assert(folded.consume("HTTP/1.1 200 OK\r\n") && !folded.consume(" ETag: \"folded\"\r\n"));
    HttpResponseHeaders unfinished;
    assert(unfinished.consume("HTTP/1.1 200 OK\r\n") && !unfinished.valid());
    std::cout << "HTTP validators: final response, bounded headers, duplicates/trailers, weak tags and injection rejection passed\n";
}
