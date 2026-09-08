#!/usr/bin/env python3
"""Export upstream checkpoints, verify ONNX parity, and emit runtime contracts.

Developer-only: requirements.txt. No Python is needed by the installed app.
Dynamic normalization/pooling uses algebra equivalent to the upstream ops.
S-KEY's probabilities are not softmaxed twice.
"""
import argparse
import copy
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
import torch
from torch import nn

BEAT_SHA = '8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331'

class DynamicNorm(nn.Module):
    def forward(self, x, shape):
        dims = tuple(range(x.ndim - len(shape), x.ndim))
        centered = x - x.mean(dim=dims, keepdim=True)
        return centered / (centered.square().mean(dim=dims, keepdim=True) + 1e-5).sqrt()

class TimePool(nn.Module):
    def forward(self, x):
        return x.mean(dim=3, keepdim=True)

class KeyPipeline(nn.Module):
    def __init__(self, hcqt, model):
        super().__init__()
        self.hcqt, self.model = hcqt, model
    def forward(self, audio):
        return self.model(self.hcqt(audio.unsqueeze(0))[:, :, :84, :])[0]

class BeatPipeline(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model
    def forward(self, spect):
        y = self.model(spect)
        return y['beat'], y['downbeat']

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cache', type=Path, default=Path('.cache/audio-analysis'))
    p.add_argument('--out', type=Path, default=Path('models/audio-analysis'))
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    torch.set_num_threads(2)
    torch.manual_seed(1928)
    rng = np.random.default_rng(1928)
    assert digest(args.cache / 'final0.ckpt') == BEAT_SHA, 'wrong Beat This! checkpoint'
    from beat_this.inference import load_model
    from beat_this.preprocessing import LogMelSpect
    beat = BeatPipeline(load_model(str(args.cache / 'final0.ckpt'), 'cpu')).eval()
    # Batch is intentionally fixed to one; the C++ host never batches windows.
    spect = torch.from_numpy(rng.normal(2, 1, (1, 500, 128)).astype('float32'))
    torch.onnx.export(beat, (spect,), str(args.out / 'beat-this-final0.onnx'),
        input_names=['spect'], output_names=['beat', 'downbeat'],
        dynamic_axes={'spect': {1: 'frames'}, 'beat': {1: 'frames'}, 'downbeat': {1: 'frames'}},
        opset_version=17, dynamo=False)
    mel = LogMelSpect()
    mel.spect_class.mel_scale.fb.numpy().astype('<f4').tofile(args.out / 'mel-filterbank.bin')
    source = args.cache / 'skey-918b83d273568d5041569bb8068843d19a335726'
    sys.path.insert(0, str(source))
    from skey.key_detection import load_model_components, key_map
    checkpoint = torch.load(source / 'skey/models/skey.pt', map_location='cpu', weights_only=False)
    hcqt, model, _ = load_model_components(checkpoint, torch.device('cpu'))
    reference = KeyPipeline(hcqt, model).eval()
    key = copy.deepcopy(reference)
    from skey.convnext import TimeDownsamplingBlock, ConvNeXtBlock
    for m in key.modules():
        if isinstance(m, (TimeDownsamplingBlock, ConvNeXtBlock)): m.norm = DynamicNorm()
    key.model.global_average_pool = TimePool()
    dummy = torch.from_numpy(rng.normal(0, .2, (1, 22050 * 10)).astype('float32'))
    torch.onnx.export(key, (dummy,), str(args.out / 'skey.onnx'), input_names=['audio'], output_names=['probs'],
        dynamic_axes={'audio': {1: 'samples'}}, opset_version=17, dynamo=False)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    sessions = {name: ort.InferenceSession(str(args.out / name), options, providers=['CPUExecutionProvider'])
                for name in ['beat-this-final0.onnx', 'skey.onnx']}
    # Reload an untouched upstream reference after ONNX tracing.
    hcqt, model, _ = load_model_components(checkpoint, torch.device('cpu'))
    reference = KeyPipeline(hcqt, model).eval()
    parity = []
    with torch.no_grad():
        for seconds in [3, 6, 10, 15, 31]:
            t = np.arange(seconds * 22050, dtype=np.float32) / 22050
            wave = sum(.2 * np.sin(2 * np.pi * hz * t) for hz in [220, 261.6256, 329.6276])
            wave += rng.normal(0, .003, len(t)).astype('float32')
            audio = torch.from_numpy(wave[None].astype('float32'))
            expected = reference(audio).numpy()
            actual = sessions['skey.onnx'].run(None, {'audio': audio.numpy()})[0]
            error = float(np.max(np.abs(expected - actual)))
            print('parity', seconds, error, flush=True)
            assert error < 1e-4 and expected.argmax() == actual.argmax(), (seconds, error)
            parity.append({'task': 'key', 'seconds': seconds, 'maxError': error, 'sameClass': True})
        for frames in [100, 777, 1500]:
            x = torch.from_numpy(rng.normal(2, 1, (1, frames, 128)).astype('float32'))
            expected = beat(x)
            actual = sessions['beat-this-final0.onnx'].run(None, {'spect': x.numpy()})
            error = max(float(np.max(np.abs(a - e.numpy()))) for a, e in zip(actual, expected))
            assert error < 3e-4, (frames, error)
            parity.append({'task': 'beat', 'frames': frames, 'maxError': error})
        # Binary fixtures verify the native log-mel implementation independently.
        waveform = rng.normal(0, .1, 22050 * 2 + 113).astype('float32')
        waveform.tofile(args.cache / 'parity-audio.f32')
        mel(torch.from_numpy(waveform)).numpy().tofile(args.cache / 'parity-mel.f32')
    contract = {
        'version': 1, 'algorithmVersion': 2,
        'beat': {'checkpoint': 'CPJKU/beat_this final0', 'sourceSha256': BEAT_SHA,
                 'file': 'beat-this-final0.onnx', 'sampleRate': 22050, 'fft': 1024, 'hop': 441,
                 'melBins': 128, 'chunkFrames': 1500, 'borderFrames': 6, 'downmix': 'mean'},
        'key': {'checkpoint': 'deezer/skey@918b83d273568d5041569bb8068843d19a335726',
                'sourceSha256': digest(source / 'skey/models/skey.pt'), 'file': 'skey.onnx', 'sampleRate': 22050,
                'minimumSeconds': 3, 'windowSeconds': 15, 'keyMap': [key_map[i] for i in range(24)],
                'secondSoftmax': False},
        'sha256': {f.name: digest(f) for f in args.out.iterdir() if f.suffix in ['.onnx', '.bin']},
        'exportParity': parity,
    }
    (args.out / 'manifest.json').write_text(json.dumps(contract, indent=2) + '\n')
    print(json.dumps(parity, indent=2))

if __name__ == '__main__': main()
