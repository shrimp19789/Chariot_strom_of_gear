# -*- coding: utf-8 -*-
"""对驱动板照片做增强 OCR, 重点读丝印: 引脚名 (EN / nSLEEP / IN1..3) 和芯片型号。

原始 OCR 对这些大尺寸实物照片效果很差, 所以这里做预处理:
  灰度 -> 放大 2~3 倍 -> CLAHE 局部对比度增强 -> 轻微锐化
并同时输出"原图"和"预处理图"的结果做对比。
"""
import os
import sys

import cv2
import numpy as np
from rapidocr_onnxruntime import RapidOCR

PROJ = r'D:\dsh\1001\ESP小车项目'
# 司机板/降压模块那几张 (17:41 拍摄的大图)
TARGETS = [
    '8d026409f15da554c0698b05e2c8f11a.jpg',
    '203057240cc96969cc30a48af07484ad.jpg',
    '60d4df2088c4f4ae2b890196908f64bf.jpg',
    '02cec176d8672e7d400d2b986632414d.jpg',
    'a9c2a7ad0204ca2d75682873badeca4e.jpg',
]

OUT = os.path.join(PROJ, '_ocr_driverboard.txt')
engine = RapidOCR()


def variants(path):
    """产出 (名字, 图像) 若干预处理版本"""
    img = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return
    yield '原图', img

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    for scale in (2, 3):
        up = cv2.resize(gray, None, fx=scale, fy=scale,
                        interpolation=cv2.INTER_CUBIC)
        clahe = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8)).apply(up)
        yield f'灰度x{scale}+CLAHE', clahe

        # 反相也试一次: 有些丝印是暗底亮字, 反相后更容易识别
        yield f'灰度x{scale}+CLAHE+反相', cv2.bitwise_not(clahe)


def ocr_image(img):
    result, _ = engine(img)
    if not result:
        return []
    items = []
    for box, text, score in result:
        ys = [p[1] for p in box]
        xs = [p[0] for p in box]
        items.append((sum(ys) / 4, min(xs), text, score))
    items.sort(key=lambda t: (t[0], t[1]))
    return items


report = []
for fn in TARGETS:
    path = os.path.join(PROJ, fn)
    if not os.path.exists(path):
        continue
    report.append(f'\n{"=" * 70}\n{fn}\n{"=" * 70}')
    for name, img in variants(path):
        items = ocr_image(img)
        # 只保留置信度尚可的, 减少噪声
        kept = [t for t in items if t[3] >= 0.5]
        report.append(f'\n--- {name}: 识别 {len(kept)} 条 ---')
        for y, x, text, score in kept:
            report.append(f'  [{score:.2f}] {text}')

with open(OUT, 'w', encoding='utf-8') as f:
    f.write('\n'.join(report))

print(f'已写入 {OUT}')
print(f'共 {len(report)} 行')
