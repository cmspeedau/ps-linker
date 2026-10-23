#!/usr/bin/env python3
import json, mimetypes, os, re, sys
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from urllib.parse import unquote,urlparse
RANGE_RE=re.compile(r'^bytes=(\\d*)-(\\d*)$'); ALLOWED={'.iso','.cso','.pbp','.zip','.7z','.rar','.pkg','.rap','.bin','.cue'}; BAD=re.compile(r'[\\x00-\\x1f\\x7f/\\\\]')
def title(n):
 s=os.path.splitext(os.path.basename(n))[0]; s=re.sub(r'\\[[^]]*\\]|\\([^)]*\\)',' ',s); s=re.sub(r'\\b(PSP|PS3|USA|EUR|JPN|DISC\\s*\\d+)\\b',' ',s,flags=re.I); return re.sub(r'[_\\.]+',' ',s).strip() or n
def load(p):
 try:
  d=json.load(open(p,encoding='utf8')); return d.get('games',d.get('items',[])) if isinstance(d,dict) else d
 except: return []
def save(p,g):
 t=p+'.tmp'; json.dump({'games':g},open(t,'w',encoding='utf8'),indent=2); os.replace(t,p)
def handler(root):
 root=os.path.realpath(root); cat=os.path.join(root,'catalog.json'); ui=os.path.join(os.path.dirname(__file__),'ui')
 class H(BaseHTTPRequestHandler):
  server_version='PSLinker/1.2'
  def js(self,code,d):
   b=json.dumps(d).encode(); self.send_response(code); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)
  def file(self,base,path):
   p=os.path.realpath(os.path.join(base,path.lstrip('/'))); br=os.path.realpath(base)
   if not(p==br or p.startswith(br+os.sep)) or not os.path.isfile(p): self.send_error(404); return
   n=os.path.getsize(p); a,e=0,max(0,n-1); m=RANGE_RE.match(self.headers.get('Range',''))
   if m and n:
    a=int(m.group(1) or max(0,n-int(m.group(2)))); e=min(int(m.group(2) or n-1),n-1)
    if a>=n or e<a:self.send_error(416);return
    self.send_response(206); self.send_header('Content-Range',f'bytes {a}-{e}/{n}')
   else:self.send_response(200)
   self.send_header('Content-Type',mimetypes.guess_type(p)[0] or 'application/octet-stream');self.send_header('Content-Length',str(e-a+1));self.end_headers()
   if self.command!='HEAD':
    with open(p,'rb') as f:f.seek(a);self.wfile.write(f.read(e-a+1))
  def do_GET(self):
   p=unquote(urlparse(self.path).path)
   if p=='/api/catalog':self.js(200,{'games':load(cat)});return
   self.file(ui,'/index.html' if p=='/' else p[3:] if p.startswith('/ui/') else p)
  def do_HEAD(self):self.do_GET()
  def do_POST(self):
   if urlparse(self.path).path!='/api/upload':self.send_error(404);return
   ct=self.headers.get('Content-Type',''); bnd=ct.split('boundary=',1)[-1].strip('"'); body=self.rfile.read(int(self.headers.get('Content-Length',0))); fields=[]; plat='psp'
   for part in body.split(('--'+bnd).encode()):
    h,_,x=part.strip(b'\r\n-').partition(b'\r\n\r\n'); ds=h.decode(errors='ignore'); nm=re.search(r'name="([^"]+)"',ds); fn=re.search(r'filename="([^"]*)"',ds)
    if not nm:continue
    if nm.group(1)=='platform':plat=x.decode().strip().lower()
    elif fn:fields.append((fn.group(1),x))
   plat=plat if plat in ('psp','ps3') else 'psp'; games=load(cat); idx={g.get('name',g.get('file')):g for g in games}; out=[]
   for raw,x in fields:
    raw=os.path.basename(raw)
    if not raw or BAD.search(raw) or os.path.splitext(raw)[1].lower() not in ALLOWED:self.js(400,{'error':'invalid filename'});return
    with open(os.path.join(root,raw),'wb') as f:f.write(x)
    it={'name':raw,'title':title(raw),'platform':plat,'path':raw,'size':len(x)};idx[raw]={**idx.get(raw,{}),**it};out.append(it)
   save(cat,list(idx.values()));self.js(201,{'uploaded':out,'games':list(idx.values())})
  def log_message(self,*a):pass
 return H
if __name__=='__main__':
 d=os.path.realpath(sys.argv[1]);os.makedirs(d,exist_ok=True);ThreadingHTTPServer(('0.0.0.0',int(sys.argv[2]) if len(sys.argv)>2 else 8080),handler(d)).serve_forever()
