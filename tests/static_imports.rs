//! Source imports are compiled into the executable instead of being read at runtime.

use std::path::PathBuf;
use std::process::Command;
use std::sync::{Mutex, MutexGuard, OnceLock};

fn compiler_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(())).lock().unwrap()
}

fn write(path: PathBuf, source: &str) {
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, source).unwrap();
}

#[test]
fn local_modules_and_packages_are_embedded() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-static-imports-{}", std::process::id()));
    let source = base.join("source");
    write(source.join("main.py"), "import helper\nfrom pack import answer\nfrom pack.sub import doubled\nprint(helper.value, answer, doubled)\n");
    write(source.join("helper.py"), "value = 7\n");
    write(source.join("pack/__init__.py"), "from .sub import doubled\nanswer = 21\n");
    write(source.join("pack/sub.py"), "from helper import value\ndoubled = value * 2\n");
    let executable = base.join("program");
    piper::compile_file(&source.join("main.py"), &executable, &Default::default()).unwrap();
    std::fs::rename(&source, base.join("source-hidden")).unwrap();
    let output = Command::new(&executable).current_dir(&base).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "7 21 14\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn imports_follow_statically_reachable_functions() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-reachable-imports-{}", std::process::id()));
    write(base.join("main.py"), "def unused():\n    import unused_dependency\ndef inner():\n    import used_dependency\n    return used_dependency.value\ndef outer():\n    return inner()\nprint(outer())\n");
    write(base.join("used_dependency.py"), "value = 42\n");
    write(base.join("unused_dependency.py"), "def nope(\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "42\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn circular_import_observes_partially_initialized_module() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-circular-imports-{}", std::process::id()));
    write(base.join("main.py"), "import left\nimport right\nprint(left.value, right.saw_value)\n");
    write(base.join("left.py"), "import right\nvalue = 12\n");
    write(base.join("right.py"), "import left\nsaw_value = hasattr(left, 'value')\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "12 False\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_standard_library_modules_need_no_python_installation() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-bundled-stdlib-{}", std::process::id()));
    write(base.join("main.py"), "import stat\nimport keyword\nimport colorsys\nimport operator\nimport copyreg\nimport bisect\nimport heapq\nimport types\nimport enum\nimport sys\nclass Color(enum.Enum):\n    RED = 1\n    BLUE = 2\nclass Mode(enum.IntFlag):\n    READ = 1\n    WRITE = 2\nvalues = [1, 3, 5]\nbisect.insort(values, 4)\nheapq.heappush(values, 0)\nprint(stat.S_ISDIR(stat.S_IFDIR), keyword.iskeyword('match'), colorsys.rgb_to_hsv(1.0, 0.0, 0.0))\nprint(operator.add(2, 3), operator.itemgetter(1)((4, 9)), callable(copyreg.pickle))\nprint(values, heapq.heappop(values), bisect.bisect_left(values, 4))\nprint(types.FunctionType.__name__, types.MethodType.__name__, types.NoneType.__name__)\nprint(types.SimpleNamespace(a=1, b=2), sys.implementation.name, sys.implementation.cache_tag)\nprint(Color.RED, Color.RED.name, Color.RED.value, list(Color), Mode.READ | Mode.WRITE)\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    std::fs::remove_file(base.join("main.py")).unwrap();
    let output = Command::new(&executable).current_dir(&base).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "True False (0.0, 1.0, 1.0)\n5 9 True\n[1, 3, 4, 5] 0 2\nfunction method NoneType\nnamespace(a=1, b=2) piper piper-314\nColor.RED RED 1 [<Color.RED: 1>, <Color.BLUE: 2>] 3\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_abc_supports_abstract_and_registered_classes() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-bundled-abc-{}", std::process::id()));
    write(base.join("main.py"), "from abc import ABC, abstractmethod, get_cache_token\nclass Shape(ABC):\n    @abstractmethod\n    def area(self):\n        pass\nclass Square(Shape):\n    def area(self):\n        return 4\nclass External:\n    pass\nbefore = get_cache_token()\nShape.register(External)\nprint(Shape.__abstractmethods__, Square().area())\nprint(issubclass(Square, Shape), isinstance(Square(), Shape))\nprint(issubclass(External, Shape), isinstance(External(), Shape), get_cache_token() > before)\ntry:\n    Shape()\nexcept TypeError as error:\n    print(type(error).__name__)\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    std::fs::remove_file(base.join("main.py")).unwrap();
    let output = Command::new(&executable).current_dir(&base).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "frozenset({'area'}) 4\nTrue True\nTrue True True\nTypeError\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_os_uses_the_native_platform_module() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-bundled-os-{}", std::process::id()));
    write(base.join("main.py"), "import os\nprint(os.path.join('one', 'two'))\nprint(os.getcwd() == os.getcwd(), os.getpid() > 0, os.cpu_count() >= 1)\nprint(bool(os.stat('.')[0]))\nprint(len(os.urandom(32)), os.urandom(32) != os.urandom(32))\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    std::fs::remove_file(base.join("main.py")).unwrap();
    let output = Command::new(&executable).current_dir(&base).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "one/two\nTrue True True\nTrue\n32 True\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_random_uses_native_entropy() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-bundled-random-{}", std::process::id()));
    write(base.join("main.py"), "import random\nvalue = random.choice(['rock', 'paper', 'scissors'])\nprint(value in ('rock', 'paper', 'scissors'))\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "True\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_io_supports_files_and_memory_streams() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-bundled-io-{}", std::process::id()));
    write(base.join("main.py"), "import io\nstream = io.StringIO()\nstream.write('piper')\nstream.seek(0)\nprint(stream.read())\nwith io.open('sample.txt', 'w') as file:\n    file.write('disk')\nwith io.open('sample.txt') as file:\n    print(file.read())\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).current_dir(&base).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "piper\ndisk\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_collections_and_iterators_execute() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-bundled-collections-{}", std::process::id()));
    write(base.join("main.py"), "from collections import Counter, namedtuple\nfrom itertools import chain, islice, pairwise, product\nPoint = namedtuple('Point', 'x y')\nprint(Counter('abac').most_common())\nprint(Point(2, 3), list(chain([1], [2, 3])))\nprint(list(islice(range(10), 2, 8, 2)), list(pairwise([1, 2, 4])))\nprint(list(product('ab', repeat=2)))\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).current_dir(&base).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "[('a', 2), ('b', 1), ('c', 1)]\nPoint(x=2, y=3) [1, 2, 3]\n[2, 4, 6] [(1, 2), (2, 4)]\n[('a', 'a'), ('a', 'b'), ('b', 'a'), ('b', 'b')]\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_json_pathlib_regex_and_codecs_execute() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-bundled-library-wave-{}", std::process::id()));
    write(base.join("main.py"), "import codecs\nimport io\nimport json\nimport pathlib\nimport re\nfrom collections import defaultdict, deque\nstream = io.StringIO()\nstream.write('piper')\nstream.seek(0)\nprint(stream.read())\nprint(json.dumps({'ready': True}, sort_keys=True))\nprint(pathlib.PurePosixPath('one') / 'two')\nprint(re.findall(r'([a-z]+)-(\\d{2,4})', 'ab-12 cd-3456 ef-1'))\nprint(re.sub(r'(\\w+)-(\\d+)', r'\\2:\\1', 'item-42'))\nprint(re.fullmatch(r'a{2,4}?b', 'aaab').group())\nprint(codecs.decode(codecs.encode('café', 'utf-8'), 'utf-8'))\nqueue = deque([2, 3], maxlen=3)\nqueue.appendleft(1)\nqueue.rotate(1)\ncounts = defaultdict(int)\nfor letter in 'aba':\n    counts[letter] += 1\nprint(queue, counts['a'], counts['missing'])\nprint(type(__import__('os').stat('.')).__name__, __import__('os').stat('.').st_size >= 0)\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).current_dir(&base).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "piper\n{\"ready\": true}\none/two\n[('ab', '12'), ('cd', '3456')]\n42:item\naaab\ncafé\ndeque([3, 1, 2], maxlen=3) 2 0\nstat_result True\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn handled_exceptions_leave_no_stale_context() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-exception-context-{}", std::process::id()));
    write(base.join("main.py"), "def probe():\n    try:\n        raise IndexError('handled')\n    except IndexError:\n        return 1\nprobe()\nraise RuntimeError('visible')\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).output().unwrap();
    assert!(!output.status.success());
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("RuntimeError: visible"), "{stderr}");
    assert!(!stderr.contains("IndexError: handled"), "{stderr}");
    assert!(!stderr.contains("During handling"), "{stderr}");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_binary_csv_and_weakref_modules_execute() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-binary-library-{}", std::process::id()));
    write(base.join("main.py"), "import base64\nimport binascii\nimport csv\nimport io\nimport struct\nimport weakref\nprint(struct.pack('>H2B4s', 0x1234, 5, 6, b'ab'), struct.unpack('>H2B4s', b'\\x12\\x34\\x05\\x06ab\\0\\0'))\nbuffer = bytearray(6)\nstruct.pack_into('<IH', buffer, 0, 0x12345678, 0x9abc)\nprint(struct.unpack_from('<IH', buffer), list(struct.iter_unpack('>H', b'\\0\\1\\0\\2')))\nprint(base64.b64encode(b'piper'), base64.b64decode(b'cGlwZXI='))\nprint(binascii.hexlify(b'abc'), binascii.unhexlify(b'616263'), hex(binascii.crc32(b'abc')))\nprint(b'abc'.translate(bytes.maketrans(b'ac', b'xz')))\nprint(list(csv.reader(['a,\"b,c\"\\n', '1,2\\n'])))\nstream = io.StringIO()\nwriter = csv.writer(stream)\nwriter.writerow(['a', 'b,c'])\nprint(repr(stream.getvalue()))\nclass Ref(weakref.ref):\n    pass\nclass Value:\n    pass\nvalue = Value()\nprint(Ref(value)() is value)\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "b'\\x124\\x05\\x06ab\\x00\\x00' (4660, 5, 6, b'ab\\x00\\x00')\n(305419896, 39612) [(1,), (2,)]\nb'cGlwZXI=' b'piper'\nb'616263' b'abc' 0x352441c2\nb'xbz'\n[['a', 'b,c'], ['1', '2']]\n'a,\"b,c\"\\r\\n'\nTrue\n");
    let _ = std::fs::remove_dir_all(base);
}

#[test]
fn bundled_introspection_context_and_logging_modules_execute() {
    let _guard = compiler_lock();
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let base = std::env::temp_dir().join(format!("piper-introspection-library-{}", std::process::id()));
    write(base.join("main.py"), "import ast\nimport contextvars\nimport hashlib\nimport logging\nimport time\nvalue = contextvars.ContextVar('value', default=3)\ntoken = value.set(9)\nprint(value.get(), contextvars.copy_context()[value])\nvalue.reset(token)\nprint(value.get())\nnode = ast.Constant(value=42)\nprint(isinstance(node, ast.AST), node.value, ast.dump(node))\nprint(hasattr(logging, 'getLogger'), len(time.strftime('%Y', time.gmtime(0))))\nprint(hashlib.sha256(b'abc').hexdigest())\nprint(hashlib.sha224(b'abc').hexdigest())\n");
    let executable = base.join("program");
    piper::compile_file(&base.join("main.py"), &executable, &Default::default()).unwrap();
    let output = Command::new(&executable).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "9 9\n3\nTrue 42 Constant(value=42)\nTrue 4\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7\n");
    let _ = std::fs::remove_dir_all(base);
}
