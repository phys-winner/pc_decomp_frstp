"""Run the production Switch radial touch handlers on the host JVM."""
import hashlib
import json
from pathlib import Path
import subprocess

from fastbuild import JAVA, ROOT
from test_android_touch_controls import extract

output = ROOT / 'build/switch-ui-20261112/touch-check'
output.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'android/app/TouchControls.java').read_text(encoding='utf-8')
methods = '\n'.join(extract(source, name) for name in ('private float switchX(', 'private float switchY(', 'private boolean switchChoose(',
                                                      'private int switchItem(', 'private boolean switchActivate(', 'private boolean switchMenu(',
                                                      'public boolean performAccessibilityAction(', 'private boolean switchTouch(', 'private void stopGamepad('))
methods = methods.replace('android.os.Bundle', 'Object')
fixture = (ROOT / 'tests/SwitchMenuCheck.java').read_text().replace('/* PRODUCTION_METHODS */', methods)
java_source = output / 'SwitchMenuCheck.java'
java_source.write_text(fixture)
subprocess.run([str(JAVA.parent / 'javac.exe'), '-encoding', 'UTF-8', '-d', str(output), str(java_source)], check=True)
result = subprocess.run([str(JAVA), '-ea', '-cp', str(output), 'SwitchMenuCheck'], capture_output=True, text=True, check=True)
report = json.loads(result.stdout)
assert report['success']
report['source_sha256'] = hashlib.sha256(source.encode('utf-8')).hexdigest()
(output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
