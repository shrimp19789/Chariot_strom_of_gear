# -*- coding: utf-8 -*-
"""OCR docs/assets 下的实物照片, 把上面的文字读出来。

背景: 当前模型不支持直接看图, 所以用 OCR 把丝印/接线标注转成文字。
"""
import os
import sys
import json

from rapidocr_onnxruntime import RapidOCR

# 仓库根 = 本文件的上上级目录; 实物照片与 OCR 文本都在 docs/assets 下
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ = os.path.join(_ROOT, 'docs', 'assets')
OUT = os.path.join(PROJ, '_ocr_result.txt')

engine = RapidOCR()
report = []

for fn in sorted(os.listdir(PROJ)):
    if not fn.lower().endswith(('.jpg', '.jpeg', '.png')):
        continue
    path = os.path.join(PROJ, fn)
    print(f'--- OCR {fn} ---')
    result, elapse = engine(path)
    if not result:
        report.append((fn, '(未识别到文字)'))
        print('   (未识别到文字)')
        continue

    # 按纵坐标分行, 同一行内按横坐标排序, 还原阅读顺序
    items = []
    for box, text, score in result:
        ys = [p[1] for p in box]
        xs = [p[0] for p in box]
        items.append({'y': sum(ys) / 4, 'x': min(xs), 'text': text,
                      'h': max(ys) - min(ys), 'score': score})
    items.sort(key=lambda d: (d['y'], d['x']))

    lines, cur, cur_y, tol = [], [], None, None
    for it in items:
        if cur_y is None:
            cur_y, tol = it['y'], max(12, it['h'] * 0.7)
        if abs(it['y'] - cur_y) > tol:
            lines.append('  '.join(cur))
            cur, cur_y, tol = [], it['y'], max(12, it['h'] * 0.7)
        cur.append(it['text'])
    if cur:
        lines.append('  '.join(cur))

    text = '\n'.join(lines)
    report.append((fn, text))
    print(text)
    print()

with open(OUT, 'w', encoding='utf-8') as f:
    for fn, text in report:
        f.write(f'{"=" * 60}\n{fn}\n{"=" * 60}\n{text}\n\n')

print(f'已写入 {OUT}')
