import tarfile, io, hashlib, os, sys
P='oes-gpu-npu'
def norm(ti):
    ti.uid=ti.gid=0; ti.uname=ti.gname='root'; return ti
buf=io.BytesIO()
with tarfile.open(fileobj=buf,mode='w:gz') as t:
    for root,dirs,files in os.walk(P+'/app'):
        dirs.sort()
        for f in sorted(files):
            fp=os.path.join(root,f); t.add(fp,arcname=os.path.relpath(fp,P+'/app'),filter=norm)
app=buf.getvalue()
m=open(P+'/manifest',encoding='utf-8').read().rstrip('\n')+'\nchecksum     = '+hashlib.md5(app).hexdigest()+'\n'
out=sys.argv[1]
with tarfile.open(out,'w:gz') as t:
    def addbytes(name,data,mode=0o644):
        ti=tarfile.TarInfo(name); ti.size=len(data); ti.mode=mode; norm(ti); t.addfile(ti,io.BytesIO(data))
    addbytes('manifest',m.encode())
    addbytes('app.tgz',app)
    for d in ['cmd','config','wizard']:
        for f in sorted(os.listdir(P+'/'+d)):
            fp=f'{P}/{d}/{f}'; t.add(fp,arcname=f'{d}/{f}',filter=norm)
    for f in ['ICON.PNG','ICON_256.PNG']: t.add(f'{P}/{f}',arcname=f,filter=norm)
print(out)
