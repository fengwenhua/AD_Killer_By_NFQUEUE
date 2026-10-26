#!/usr/bin/env python3
"""Offline protocol checks: release credentials never reach a download host."""
import io
import http.client
import json
from pathlib import Path
import sys
import unittest
import urllib.error
import urllib.request
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import publish_release as release


class Response(io.BytesIO):
    url='https://storage.example/asset'


class TransportTests(unittest.TestCase):
    def test_read_only_client_blocks_mutations_before_network(self):
        github=release.GitHub('synthetic-secret',read_only=True)
        with patch.object(urllib.request,'build_opener') as opener:
            with self.assertRaisesRegex(ValueError,'read-only'): github.request('/releases',{'draft':True},'POST')
            with self.assertRaisesRegex(ValueError,'read-only'): github.upload(10,'name',b'bytes')
            opener.assert_not_called()

    def test_ci_artifact_download_uses_the_rest_media_type(self):
        requests=[]
        class Opener:
            def open(self,request,timeout): requests.append(request); return Response(b'CI archive')
        with patch.object(urllib.request,'build_opener',return_value=Opener()):
            self.assertEqual(release.GitHub('synthetic-secret').artifact(8),b'CI archive')
        self.assertEqual(requests[0].get_header('Accept'),'application/vnd.github+json')

    def test_asset_redirect_drops_authorization_and_requests_binary(self):
        requests=[]
        class Opener:
            def __init__(self,authenticated): self.authenticated=authenticated
            def open(self,request,timeout):
                requests.append(request)
                if self.authenticated:
                    raise urllib.error.HTTPError(request.full_url,302,'redirect',{'Location':'https://storage.example/asset'},None)
                return Response(b'original bytes')
        def opener(handler): return Opener(handler is release.NoRedirect or isinstance(handler,release.NoRedirect))
        with patch.object(urllib.request,'build_opener',side_effect=opener):
            self.assertEqual(release.GitHub('synthetic-secret').asset(100),b'original bytes')
        api,download=({key.lower():value for key,value in request.header_items()} for request in requests)
        self.assertEqual(api['authorization'],'Bearer synthetic-secret')
        self.assertEqual(api['accept'],'application/octet-stream')
        self.assertNotIn('authorization',download)

    def test_public_redirect_cannot_downgrade_https(self):
        with self.assertRaisesRegex(ValueError,'insecure'):
            release.HTTPSRedirect().redirect_request(urllib.request.Request('https://storage.example/a'),None,302,'',{},'http://storage.example/b')

    def test_api_json_rejects_duplicate_fields(self):
        class Opener:
            def open(self,*args,**kwargs): return Response(b'{"id":1,"id":2}')
        with patch.object(urllib.request,'build_opener',return_value=Opener()),self.assertRaisesRegex(ValueError,'duplicate'):
            release.GitHub('synthetic-secret').request('/releases/1')

    def test_mutating_transport_failure_is_not_retried(self):
        calls=[]
        class Opener:
            def open(self,request,**kwargs): calls.append(request); raise TimeoutError('lost response')
        with patch.object(urllib.request,'build_opener',return_value=Opener()),self.assertRaises(TimeoutError):
            release.GitHub('synthetic-secret').request('/releases',{'draft':True},'POST')
        self.assertEqual(len(calls),1)

    def test_partial_http_reply_is_an_uncertain_transport_failure(self):
        class Partial(Response):
            def read1(self,*args): raise http.client.IncompleteRead(b'partial JSON')
        class Opener:
            def open(self,*args,**kwargs): return Partial(b'')
        with patch.object(urllib.request,'build_opener',return_value=Opener()) as factory,self.assertRaises(OSError):
            release.GitHub('synthetic-secret').request('/releases',{'draft':True},'POST')
        self.assertEqual(factory.call_count,1)

    def test_initial_bad_status_is_normalized_at_all_transfer_boundaries(self):
        class Opener:
            def open(self,*args,**kwargs): raise http.client.BadStatusLine('broken response')
        github=release.GitHub('synthetic-secret')
        for operation in (lambda:github.request('/releases'),lambda:github.asset(100),
                          lambda:github.public_download('https://storage.example/asset'),lambda:github.upload(10,'file',b'bytes')):
            with self.subTest(operation=operation),patch.object(urllib.request,'build_opener',return_value=Opener()) as factory,self.assertRaises(OSError): operation()
            self.assertEqual(factory.call_count,1)

    def test_response_body_cap_and_expired_readback(self):
        github=release.GitHub('synthetic-secret')
        with patch.object(release,'MAX_DOWNLOAD',4):
            self.assertEqual(github.read(Response(b'1234')),b'1234')
            with self.assertRaisesRegex(ValueError,'size limit'): github.read(Response(b'12345'))
        github.deadline=0
        with self.assertRaises(TimeoutError): github.read(Response(b'123'))


if __name__=='__main__': unittest.main()
