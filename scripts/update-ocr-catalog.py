import concurrent.futures, hashlib, json, pathlib, urllib.request
revision = '87416418657359cb625c412a48b6e1d6d41c29bd'
languages = [('eng','English'),('spa','Spanish'),('rus','Russian'),('ukr','Ukrainian'),('deu','German'),('fra','French'),('ita','Italian'),('por','Portuguese'),('pol','Polish'),('chi_sim','Chinese · Simplified'),('chi_tra','Chinese · Traditional'),('jpn','Japanese'),('kor','Korean'),('ara','Arabic'),('hin','Hindi'),('tur','Turkish'),('vie','Vietnamese'),('tha','Thai'),('nld','Dutch'),('ces','Czech'),('ell','Greek'),('heb','Hebrew'),('fas','Persian'),('swe','Swedish')]
def fetch(item):
    code, name = item
    data = urllib.request.urlopen(f'https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/{revision}/{code}.traineddata', timeout=60).read()
    return {'code':code, 'name':name, 'bytes':len(data), 'sha256':hashlib.sha256(data).hexdigest()}
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    packs=list(pool.map(fetch,languages))
pathlib.Path('assets/ocr-packs.json').write_text(json.dumps({'revision':revision,'packs':packs},ensure_ascii=False,indent=2)+'\n')
print(f'Pinned {len(packs)} OCR packs; {sum(p["bytes"] for p in packs)/1e6:.1f} MB available on demand.')
