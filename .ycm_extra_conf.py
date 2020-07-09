def Settings(**kwargs):
  return {
    'flags': [
        '-std=gnu11',
        '-D_GNU_SOURCE',
        '-Iinclude',
        '-Ibuild',
        '-Ibuild/wvncc@exe/',
        '-I/usr/include/pixman-1',
        '-Isubprojects/aml/include',
        '-Wall',
        '-Wextra',
        '-Wno-unused-parameter'
        ],
  }
